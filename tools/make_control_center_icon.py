"""生成原生控制中心应用图标（多尺寸 ICO）。

设计语言取自 native/control_center/app/cc_theme.hpp 的暖黑金配色：
深暖色圆角底 + 金色框架/线条 + 玉色强调，和窗口标题栏的菱形 logo 保持一致。

用法：
  python make_control_center_icon.py preview <输出目录>   生成候选预览图
  python make_control_center_icon.py build <输出 ico>     生成最终多尺寸 ico
"""

import math
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

# 主题色（RGB）
BG_TOP = (46, 36, 22)
BG_BOTTOM = (20, 15, 10)
GOLD = (200, 164, 92)
GOLD_BRIGHT = (227, 200, 132)
GOLD_DIM = (90, 74, 46)
JADE = (109, 171, 134)
JADE_BRIGHT = (159, 217, 184)
JADE_DEEP = (29, 42, 34)

SS = 4  # 超采样倍数


def _lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def _rounded_mask(size, box, radius, scale):
    mask = Image.new('L', (size * scale, size * scale), 0)
    draw = ImageDraw.Draw(mask)
    x0, y0, x1, y1 = [int(round(v * scale)) for v in box]
    draw.rounded_rectangle((x0, y0, x1, y1), radius=int(round(radius * scale)), fill=255)
    return mask.resize((size, size), Image.LANCZOS)


def _tile(size):
    """深色圆角底座（含纵向渐变与内侧暖色边）。"""
    scale = SS
    big = size * scale
    gradient = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    draw = ImageDraw.Draw(gradient)
    for y in range(big):
        t = y / max(1, big - 1)
        color = _lerp(BG_TOP, BG_BOTTOM, t)
        draw.line([(0, y), (big, y)], fill=color + (255,))
    # 左上角柔光，避免纯平。
    glow = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    gdraw = ImageDraw.Draw(glow)
    gdraw.ellipse((-big * 0.35, -big * 0.55, big * 0.95, big * 0.75), fill=GOLD_DIM + (90,))
    glow = glow.filter(ImageFilter.GaussianBlur(big * 0.12))
    gradient = Image.alpha_composite(gradient, glow)
    box = (0.055 * size, 0.055 * size, 0.945 * size, 0.945 * size)
    radius = 0.235 * size
    mask = _rounded_mask(size, box, radius, scale)
    out = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    out.paste(gradient.resize((size, size), Image.LANCZOS), (0, 0), mask)
    # 内侧 1px 描边（暗金）。
    edge = Image.new('RGBA', (size * scale, size * scale), (0, 0, 0, 0))
    edraw = ImageDraw.Draw(edge)
    inset = 0.055 * size * scale
    edraw.rounded_rectangle(
        (inset, inset, big - inset, big - inset),
        radius=int(round(radius * scale)),
        outline=GOLD_DIM + (170,),
        width=max(1, int(round(size * scale * 0.006))),
    )
    out = Image.alpha_composite(out, edge.resize((size, size), Image.LANCZOS))
    return out


def _spark(draw, cx, cy, outer, inner, fill):
    """四角星（凹边菱形星），比普通菱形更有光感。"""
    points = []
    for i in range(8):
        angle = math.pi / 2 * 0  # 顶部起始
        theta = -math.pi / 2 + i * math.pi / 4
        radius = outer if i % 2 == 0 else inner
        points.append((cx + math.cos(theta) * radius, cy + math.sin(theta) * radius))
    draw.polygon(points, fill=fill)


def _stroke(size, factor, minimum):
    """按目标尺寸计算线宽：小尺寸保证至少 minimum 像素，避免发虚。"""
    return max(size * factor, minimum)


def _draw_frame_spark(size):
    """方案 A：金色圆角框架 + 中心玉色星芒。"""
    scale = SS
    big = size * scale
    unit = big / 1024.0
    img = _tile(size).resize((big, big), Image.LANCZOS)
    draw = ImageDraw.Draw(img)
    frame = (0.255 * big, 0.255 * big, 0.745 * big, 0.745 * big)
    width = max(1, int(round(_stroke(size, 0.042, 1.6) * scale)))
    draw.rounded_rectangle(frame, radius=int(0.115 * big), outline=GOLD + (235,), width=width)
    # 内层淡金细框，仅中大尺寸保留，避免小图标发糊。
    if size >= 32:
        inner = (0.305 * big, 0.305 * big, 0.695 * big, 0.695 * big)
        draw.rounded_rectangle(inner, radius=int(0.082 * big), outline=GOLD_BRIGHT + (110,),
                              width=max(1, int(round(_stroke(size, 0.012, 1.0) * scale))))
    glow = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    gdraw = ImageDraw.Draw(glow)
    gdraw.ellipse((0.36 * big, 0.36 * big, 0.64 * big, 0.64 * big), fill=JADE + (150,))
    glow = glow.filter(ImageFilter.GaussianBlur(0.045 * big))
    img = Image.alpha_composite(img, glow)
    draw = ImageDraw.Draw(img)
    star_outer = _stroke(size, 0.165, 2.6) * scale
    star_inner = _stroke(size, 0.040, 0.9) * scale
    _spark(draw, 0.5 * big, 0.5 * big, star_outer, star_inner, JADE_BRIGHT + (255,))
    if size >= 32:
        dot = 0.028 * big
        draw.ellipse((0.5 * big - dot, 0.5 * big - dot, 0.5 * big + dot, 0.5 * big + dot),
                     fill=(255, 255, 255, 235))
    return img.resize((size, size), Image.LANCZOS)


def _draw_zoom_stack(size):
    """方案 B：三级放大方框，右下到左上递进。"""
    scale = SS
    big = size * scale
    img = _tile(size).resize((big, big), Image.LANCZOS)
    draw = ImageDraw.Draw(img)
    boxes = [
        ((0.255, 0.545, 0.470, 0.760), GOLD_DIM + (235,), 0.010),
        ((0.330, 0.400, 0.620, 0.690), GOLD + (240,), 0.016),
        ((0.405, 0.250, 0.770, 0.615), GOLD_BRIGHT + (250,), 0.022),
    ]
    for (x0, y0, x1, y1), color, w in boxes:
        draw.rounded_rectangle((x0 * big, y0 * big, x1 * big, y1 * big),
                               radius=int(0.070 * big), outline=color, width=max(1, int(round(w * big))))
    glow = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    gdraw = ImageDraw.Draw(glow)
    gdraw.rounded_rectangle((0.505 * big, 0.355 * big, 0.665 * big, 0.515 * big),
                            radius=int(0.030 * big), fill=JADE + (190,))
    glow = glow.filter(ImageFilter.GaussianBlur(0.035 * big))
    img = Image.alpha_composite(img, glow)
    draw = ImageDraw.Draw(img)
    draw.rounded_rectangle((0.505 * big, 0.355 * big, 0.665 * big, 0.515 * big),
                           radius=int(0.030 * big), outline=JADE_BRIGHT + (250,),
                           width=max(1, int(round(0.012 * big))))
    return img.resize((size, size), Image.LANCZOS)


def _draw_bars(size):
    """方案 C：递增柱状（性能/补帧），顶条玉色。"""
    scale = SS
    big = size * scale
    img = _tile(size).resize((big, big), Image.LANCZOS)
    draw = ImageDraw.Draw(img)
    heights = [0.26, 0.38, 0.50, 0.62]
    colors = [GOLD_DIM + (255,), GOLD + (255,), GOLD_BRIGHT + (255,), JADE_BRIGHT + (255,)]
    left = 0.275
    width = 0.105
    gap = 0.042
    for i, (h, color) in enumerate(zip(heights, colors)):
        x0 = (left + i * (width + gap)) * big
        x1 = x0 + width * big
        y1 = 0.735 * big
        y0 = y1 - h * big
        draw.rounded_rectangle((x0, y0, x1, y1), radius=int(0.045 * big), fill=color)
    glow = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    gdraw = ImageDraw.Draw(glow)
    gdraw.rounded_rectangle((0.275 * big, 0.735 * big - (0.62 + 0.05) * big,
                            0.275 * big + 0.105 * big, 0.70 * big), radius=int(0.045 * big),
                            fill=JADE + (160,))
    glow = glow.filter(ImageFilter.GaussianBlur(0.040 * big))
    img = Image.alpha_composite(img, glow)
    return img.resize((size, size), Image.LANCZOS)


def _diamond_ring(big, cx, cy, outer_r, inner_r):
    """返回菱形环的 L 掩码（外菱形减内菱形），比折线描边更清晰。"""
    from PIL import ImageChops
    outer = Image.new('L', (big, big), 0)
    ImageDraw.Draw(outer).polygon(
        [(cx, cy - outer_r), (cx + outer_r, cy), (cx, cy + outer_r), (cx - outer_r, cy)], fill=255)
    inner = Image.new('L', (big, big), 0)
    ImageDraw.Draw(inner).polygon(
        [(cx, cy - inner_r), (cx + inner_r, cy), (cx, cy + inner_r), (cx - inner_r, cy)], fill=255)
    return ImageChops.subtract(outer, inner)


def _draw_diamond(size):
    """方案 D：金色菱形（呼应标题栏 logo）+ 玉色内芯。"""
    scale = SS
    big = size * scale
    img = _tile(size).resize((big, big), Image.LANCZOS)
    cx, cy = 0.5 * big, 0.5 * big
    stroke = _stroke(size, 0.048 if size < 32 else 0.042, 2.0) * scale
    outer_r = 0.300 * big

    # 菱形外圈柔光，让图标在深色任务栏上有轻微发光感；小尺寸省略以免发糊。
    if size >= 32:
        halo = Image.new('RGBA', (big, big), (0, 0, 0, 0))
        mask_halo = _diamond_ring(big, cx, cy, outer_r + 0.045 * big, 0.0)
        halo.paste(GOLD + (120,), (0, 0), mask_halo)
        halo = halo.filter(ImageFilter.GaussianBlur(0.035 * big))
        img = Image.alpha_composite(img, halo)

    ring = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ring.paste(GOLD + (245,), (0, 0), _diamond_ring(big, cx, cy, outer_r, outer_r - stroke))
    img = Image.alpha_composite(img, ring)
    if size >= 48:
        accent = Image.new('RGBA', (big, big), (0, 0, 0, 0))
        accent.paste(GOLD_BRIGHT + (120,), (0, 0),
                     _diamond_ring(big, cx, cy, outer_r - 0.035 * big, outer_r - 0.035 * big - 0.012 * big))
        img = Image.alpha_composite(img, accent)

    glow = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    gdraw = ImageDraw.Draw(glow)
    gdraw.ellipse((cx - 0.13 * big, cy - 0.13 * big, cx + 0.13 * big, cy + 0.13 * big),
                  fill=JADE + (170,))
    glow = glow.filter(ImageFilter.GaussianBlur(0.05 * big))
    img = Image.alpha_composite(img, glow)
    draw = ImageDraw.Draw(img)
    star_outer = _stroke(size, 0.130, 2.6) * scale
    star_inner = _stroke(size, 0.032, 0.9) * scale
    _spark(draw, cx, cy, star_outer, star_inner, JADE_BRIGHT + (255,))
    if size >= 32:
        dot = 0.022 * big
        draw.ellipse((cx - dot, cy - dot, cx + dot, cy + dot), fill=(255, 255, 255, 240))
    return img.resize((size, size), Image.LANCZOS)


def _diamond_mask(big, cx, cy, r):
    """单个菱形 L 掩码（r 为顶点到中心的距离）。"""
    mask = Image.new('L', (big, big), 0)
    ImageDraw.Draw(mask).polygon(
        [(cx, cy - r), (cx + r, cy), (cx, cy + r), (cx - r, cy)], fill=255)
    return mask


def _jade_gradient_diamond(big, cx, cy, r):
    """玉色实心菱形：自上而下由亮到深的渐变，边缘略暗以便在深底上分层。"""
    gradient = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    gdraw = ImageDraw.Draw(gradient)
    top, bottom = int(cy - r), int(cy + r)
    for y in range(top, bottom + 1):
        t = (y - top) / max(1, bottom - top)
        color = _lerp(JADE_BRIGHT, JADE, min(1.0, t * 1.10))
        gdraw.line([(0, y), (big, y)], fill=color + (255,))
    out = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    out.paste(gradient, (0, 0), _diamond_mask(big, cx, cy, r))
    return out


def _ring_from_mask(big, cx, cy, outer_r, inner_r, color):
    from PIL import ImageChops
    def mask(r):
        return _diamond_mask(big, cx, cy, r)
    ring = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ring.paste(color, (0, 0), ImageChops.subtract(mask(outer_r), mask(inner_r)))
    return ring


def _draw_diamond_big(size):
    """方案 E：放大版菱形环，顶点更靠外、星芒更大，整体更饱满。"""
    scale = SS
    big = size * scale
    img = _tile(size).resize((big, big), Image.LANCZOS)
    cx, cy = 0.5 * big, 0.5 * big
    stroke = _stroke(size, 0.060 if size < 32 else 0.048, 2.2) * scale
    outer_r = 0.360 * big
    if size >= 32:
        halo = Image.new('RGBA', (big, big), (0, 0, 0, 0))
        halo.paste(GOLD + (115,), (0, 0), _diamond_ring(big, cx, cy, outer_r + 0.042 * big, 0.0))
        halo = halo.filter(ImageFilter.GaussianBlur(0.038 * big))
        img = Image.alpha_composite(img, halo)
    img = Image.alpha_composite(img, _ring_from_mask(big, cx, cy, outer_r, outer_r - stroke, GOLD + (250,)))
    if size >= 48:
        inner_r = outer_r - 0.040 * big
        img = Image.alpha_composite(img, _ring_from_mask(
            big, cx, cy, inner_r, inner_r - 0.010 * big, GOLD_BRIGHT + (105,)))
    glow = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((cx - 0.15 * big, cy - 0.15 * big, cx + 0.15 * big, cy + 0.15 * big),
                                 fill=JADE + (185,))
    glow = glow.filter(ImageFilter.GaussianBlur(0.055 * big))
    img = Image.alpha_composite(img, glow)
    draw = ImageDraw.Draw(img)
    star_outer = _stroke(size, 0.180, 3.0) * scale
    star_inner = _stroke(size, 0.042, 1.0) * scale
    _spark(draw, cx, cy, star_outer, star_inner, JADE_BRIGHT + (255,))
    if size >= 32:
        dot = 0.026 * big
        draw.ellipse((cx - dot, cy - dot, cx + dot, cy + dot), fill=(255, 255, 255, 245))
    return img.resize((size, size), Image.LANCZOS)


def _draw_jade_solid(size):
    """方案 F：金色菱形环 + 玉色实心菱形（更大更快读，16px 也成块）。"""
    scale = SS
    big = size * scale
    img = _tile(size).resize((big, big), Image.LANCZOS)
    cx, cy = 0.5 * big, 0.5 * big
    outer_r = 0.372 * big
    stroke = _stroke(size, 0.058 if size < 32 else 0.046, 2.2) * scale
    glow = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((cx - 0.20 * big, cy - 0.20 * big, cx + 0.20 * big, cy + 0.20 * big),
                                 fill=JADE + (130,))
    glow = glow.filter(ImageFilter.GaussianBlur(0.06 * big))
    img = Image.alpha_composite(img, glow)
    solid_r = 0.255 * big
    img = Image.alpha_composite(img, _jade_gradient_diamond(big, cx, cy, solid_r))
    # 实心菱形的暗玉描边，把玉色与金环分开。
    img = Image.alpha_composite(img, _ring_from_mask(big, cx, cy, solid_r, solid_r - 0.014 * big,
                                                     JADE_DEEP + (230,)))
    img = Image.alpha_composite(img, _ring_from_mask(big, cx, cy, outer_r, outer_r - stroke, GOLD + (250,)))
    draw = ImageDraw.Draw(img)
    # 内芯小星芒（金色），在玉色上形成中心亮点。
    star_outer = _stroke(size, 0.122, 2.4) * scale
    star_inner = _stroke(size, 0.026, 0.8) * scale
    _spark(draw, cx, cy, star_outer, star_inner, GOLD_BRIGHT + (255,))
    if size >= 32:
        dot = 0.022 * big
        draw.ellipse((cx - dot, cy - dot, cx + dot, cy + dot), fill=(255, 255, 255, 235))
    return img.resize((size, size), Image.LANCZOS)


def _draw_nested_rings(size):
    """方案 G：金色外菱环 + 玉色内菱环 + 中心星芒，双层几何。"""
    scale = SS
    big = size * scale
    img = _tile(size).resize((big, big), Image.LANCZOS)
    cx, cy = 0.5 * big, 0.5 * big
    outer_r = 0.352 * big
    outer_stroke = _stroke(size, 0.052 if size < 32 else 0.042, 2.0) * scale
    img = Image.alpha_composite(img, _ring_from_mask(big, cx, cy, outer_r, outer_r - outer_stroke,
                                                     GOLD + (250,)))
    if size >= 24:
        inner_r = 0.205 * big
        inner_stroke = _stroke(size, 0.030, 1.4) * scale
        inner = _ring_from_mask(big, cx, cy, inner_r, inner_r - inner_stroke, JADE_BRIGHT + (215,))
        img = Image.alpha_composite(img, inner)
    glow = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((cx - 0.11 * big, cy - 0.11 * big, cx + 0.11 * big, cy + 0.11 * big),
                                 fill=JADE_BRIGHT + (150,))
    glow = glow.filter(ImageFilter.GaussianBlur(0.04 * big))
    img = Image.alpha_composite(img, glow)
    draw = ImageDraw.Draw(img)
    star_outer = _stroke(size, 0.098, 2.0) * scale
    star_inner = _stroke(size, 0.026, 0.8) * scale
    _spark(draw, cx, cy, star_outer, star_inner, GOLD_BRIGHT + (255,))
    return img.resize((size, size), Image.LANCZOS)


def _octagon_mask(big, cx, cy, r):
    """正八边形 L 掩码（切角菱形，读起来像宝石/盾形）。"""
    points = []
    for i in range(8):
        theta = -math.pi / 2 + i * math.pi / 4
        points.append((cx + math.cos(theta) * r, cy + math.sin(theta) * r))
    mask = Image.new('L', (big, big), 0)
    ImageDraw.Draw(mask).polygon(points, fill=255)
    return mask


def _draw_gem_spark(size):
    """方案 H：切角菱形（八边形）金环 + 大玉色星芒，轮廓更特别。"""
    from PIL import ImageChops
    scale = SS
    big = size * scale
    img = _tile(size).resize((big, big), Image.LANCZOS)
    cx, cy = 0.5 * big, 0.5 * big
    r = 0.362 * big
    stroke = _stroke(size, 0.056 if size < 32 else 0.046, 2.2) * scale
    ring = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ring.paste(GOLD + (250,), (0, 0),
               ImageChops.subtract(_octagon_mask(big, cx, cy, r), _octagon_mask(big, cx, cy, r - stroke)))
    if size >= 32:
        halo = Image.new('RGBA', (big, big), (0, 0, 0, 0))
        halo.paste(GOLD + (110,), (0, 0), _octagon_mask(big, cx, cy, r + 0.030 * big))
        halo = halo.filter(ImageFilter.GaussianBlur(0.040 * big))
        img = Image.alpha_composite(img, halo)
    img = Image.alpha_composite(img, ring)
    if size >= 48:
        accent = Image.new('RGBA', (big, big), (0, 0, 0, 0))
        accent.paste(GOLD_BRIGHT + (95,), (0, 0), ImageChops.subtract(
            _octagon_mask(big, cx, cy, r - 0.045 * big), _octagon_mask(big, cx, cy, r - 0.056 * big)))
        img = Image.alpha_composite(img, accent)
    glow = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((cx - 0.17 * big, cy - 0.17 * big, cx + 0.17 * big, cy + 0.17 * big),
                                 fill=JADE + (180,))
    glow = glow.filter(ImageFilter.GaussianBlur(0.060 * big))
    img = Image.alpha_composite(img, glow)
    draw = ImageDraw.Draw(img)
    star_outer = _stroke(size, 0.200, 3.0) * scale
    star_inner = _stroke(size, 0.046, 1.0) * scale
    _spark(draw, cx, cy, star_outer, star_inner, JADE_BRIGHT + (255,))
    if size >= 32:
        dot = 0.028 * big
        draw.ellipse((cx - dot, cy - dot, cx + dot, cy + dot), fill=(255, 255, 255, 245))
    return img.resize((size, size), Image.LANCZOS)


def _font(size_px):
    """粗体字模（用于字母徽标）；缺字体时退回默认位图字体。"""
    try:
        return ImageFont.truetype('C:/Windows/Fonts/arialbd.ttf', size_px)
    except OSError:
        return ImageFont.load_default()


def _draw_monogram(size):
    """方案 I：金色字母徽标（N）+ 玉色星芒点缀，走品牌字标路线。"""
    scale = SS
    big = size * scale
    img = _tile(size).resize((big, big), Image.LANCZOS)
    cx, cy = 0.5 * big, 0.5 * big
    font = _font(int(0.720 * big))
    text = 'N'
    box = font.getbbox(text)
    tw, th = box[2] - box[0], box[3] - box[1]
    tx = cx - tw / 2 - box[0]
    ty = cy - th / 2 - box[1]
    glow = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    gdraw = ImageDraw.Draw(glow)
    gdraw.text((tx, ty), text, font=font, fill=GOLD_BRIGHT + (140,))
    glow = glow.filter(ImageFilter.GaussianBlur(0.030 * big))
    img = Image.alpha_composite(img, glow)
    draw = ImageDraw.Draw(img)
    draw.text((tx, ty), text, font=font, fill=GOLD + (255,))
    # 星芒点在上标位置，给字标一点玉色记忆点。
    star_outer = _stroke(size, 0.115, 2.0) * scale
    star_inner = _stroke(size, 0.030, 0.8) * scale
    _spark(draw, 0.760 * big, 0.255 * big, star_outer, star_inner, JADE_BRIGHT + (255,))
    if size >= 32:
        dot = 0.020 * big
        draw.ellipse((0.760 * big - dot, 0.255 * big - dot,
                      0.760 * big + dot, 0.255 * big + dot), fill=(255, 255, 255, 235))
    return img.resize((size, size), Image.LANCZOS)


def _gear_mask(big, cx, cy, outer_r, teeth=8, tooth_h=0.070, tooth_w=0.125):
    """齿轮 L 掩码：基圆 + 均布矩形齿。"""
    mask = Image.new('L', (big, big), 0)
    draw = ImageDraw.Draw(mask)
    draw.ellipse((cx - outer_r, cy - outer_r, cx + outer_r, cy + outer_r), fill=255)
    for i in range(teeth):
        theta = -math.pi / 2 + i * 2 * math.pi / teeth
        tx = cx + math.cos(theta) * (outer_r + tooth_h * big * 0.35)
        ty = cy + math.sin(theta) * (outer_r + tooth_h * big * 0.35)
        half_w = tooth_w * big * 0.5
        half_h = tooth_h * big * 0.62
        corners = []
        for sx, sy in ((-half_w, -half_h), (half_w, -half_h), (half_w, half_h), (-half_w, half_h)):
            corners.append((tx + sx * math.cos(theta) - sy * math.sin(theta),
                            ty + sx * math.sin(theta) + sy * math.cos(theta)))
        draw.polygon(corners, fill=255)
    return mask


def _draw_gear(size):
    """方案 J：齿轮（调节/控制中心）+ 玉色内芯。"""
    from PIL import ImageChops
    scale = SS
    big = size * scale
    img = _tile(size).resize((big, big), Image.LANCZOS)
    cx, cy = 0.5 * big, 0.5 * big
    gear = _gear_mask(big, cx, cy, 0.288 * big)
    hole = Image.new('L', (big, big), 0)
    ImageDraw.Draw(hole).ellipse((cx - 0.222 * big, cy - 0.222 * big,
                                  cx + 0.222 * big, cy + 0.222 * big), fill=255)
    ring = ImageChops.subtract(gear, hole)
    gold = Image.new('RGBA', (big, big), GOLD + (250,))
    img = Image.alpha_composite(img, _paste_masked(gold, ring))
    jade_r = 0.172 * big
    jade = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(jade).ellipse((cx - jade_r, cy - jade_r, cx + jade_r, cy + jade_r),
                                 fill=JADE + (255,))
    img = Image.alpha_composite(img, jade)
    draw = ImageDraw.Draw(img)
    star_outer = _stroke(size, 0.115, 2.2) * scale
    star_inner = _stroke(size, 0.030, 0.9) * scale
    _spark(draw, cx, cy, star_outer, star_inner, JADE_BRIGHT + (255,))
    if size >= 32:
        dot = 0.022 * big
        draw.ellipse((cx - dot, cy - dot, cx + dot, cy + dot), fill=(255, 255, 255, 235))
    return img.resize((size, size), Image.LANCZOS)


def _paste_masked(source, mask):
    """把 RGBA source 按 L mask 抠出后返回新图层。"""
    layer = Image.new('RGBA', source.size, (0, 0, 0, 0))
    layer.paste(source, (0, 0), mask)
    return layer


def _draw_pulse(size):
    """方案 K：心电图/帧率脉冲线，峰值玉色高亮。"""
    scale = SS
    big = size * scale
    img = _tile(size).resize((big, big), Image.LANCZOS)
    points = [(0.115, 0.520), (0.280, 0.520), (0.352, 0.300), (0.440, 0.728),
              (0.545, 0.395), (0.628, 0.520), (0.885, 0.520)]
    width = _stroke(size, 0.070 if size < 32 else 0.056, 2.2) * scale
    line = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(line).line([(x * big, y * big) for x, y in points],
                              fill=GOLD + (255,), width=int(round(width)), joint='curve')
    glow = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(glow).line([(x * big, y * big) for x, y in points],
                              fill=JADE + (150,), width=int(round(width * 1.9)), joint='curve')
    glow = glow.filter(ImageFilter.GaussianBlur(0.045 * big))
    img = Image.alpha_composite(img, glow)
    img = Image.alpha_composite(img, line)
    draw = ImageDraw.Draw(img)
    px, py = 0.545 * big, 0.395 * big
    dot = 0.030 * big
    draw.ellipse((px - dot, py - dot, px + dot, py + dot), fill=JADE_BRIGHT + (255,))
    if size >= 32:
        core = 0.014 * big
        draw.ellipse((px - core, py - core, px + core, py + core), fill=(255, 255, 255, 245))
    return img.resize((size, size), Image.LANCZOS)


def _draw_gauge(size):
    """方案 L：仪表盘弧线 + 玉色指针，控制/性能仪表语义。"""
    scale = SS
    big = size * scale
    img = _tile(size).resize((big, big), Image.LANCZOS)
    cx, cy = 0.5 * big, 0.560 * big
    r = 0.330 * big
    width = _stroke(size, 0.075 if size < 32 else 0.058, 2.4) * scale
    box = (cx - r, cy - r, cx + r, cy + r)
    arc = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(arc).arc(box, start=150, end=390, fill=GOLD + (255,), width=int(round(width)))
    img = Image.alpha_composite(img, arc)
    if size >= 32:
        ticks = Image.new('RGBA', (big, big), (0, 0, 0, 0))
        tdraw = ImageDraw.Draw(ticks)
        for k in range(5):
            ang = math.radians(150 + k * 60)
            x0 = cx + math.cos(ang) * (r - width * 1.35)
            y0 = cy + math.sin(ang) * (r - width * 1.35)
            x1 = cx + math.cos(ang) * (r - width * 0.35)
            y1 = cy + math.sin(ang) * (r - width * 0.35)
            tdraw.line([(x0, y0), (x1, y1)], fill=GOLD_BRIGHT + (190,),
                       width=max(1, int(round(0.014 * big))))
        img = Image.alpha_composite(img, ticks)
    ang = math.radians(-45)
    nx = cx + math.cos(ang) * (r - width * 1.15)
    ny = cy + math.sin(ang) * (r - width * 1.15)
    needle = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(needle).line([(cx, cy), (nx, ny)], fill=JADE_BRIGHT + (255,),
                                width=max(1, int(round(_stroke(size, 0.048, 1.8) * scale))))
    img = Image.alpha_composite(img, needle)
    draw = ImageDraw.Draw(img)
    hub = 0.058 * big
    draw.ellipse((cx - hub, cy - hub, cx + hub, cy + hub), fill=GOLD + (255,))
    inner = 0.026 * big
    draw.ellipse((cx - inner, cy - inner, cx + inner, cy + inner), fill=(255, 255, 255, 235))
    return img.resize((size, size), Image.LANCZOS)


def _draw_shield(size):
    """方案 M：护盾（保护/恢复语义）+ 玉色星芒内芯。"""
    from PIL import ImageChops
    scale = SS
    big = size * scale
    img = _tile(size).resize((big, big), Image.LANCZOS)
    cx, cy = 0.5 * big, 0.5 * big
    outer = [(0.500, 0.105), (0.838, 0.235), (0.838, 0.520), (0.712, 0.748),
             (0.500, 0.900), (0.288, 0.748), (0.162, 0.520), (0.162, 0.235)]
    inner = [(0.500 + (x - 0.5) * 0.760, 0.500 + (y - 0.5) * 0.760) for x, y in outer]
    om = Image.new('L', (big, big), 0)
    ImageDraw.Draw(om).polygon([(x * big, y * big) for x, y in outer], fill=255)
    im = Image.new('L', (big, big), 0)
    ImageDraw.Draw(im).polygon([(x * big, y * big) for x, y in inner], fill=255)
    gold = Image.new('RGBA', (big, big), GOLD + (250,))
    img = Image.alpha_composite(img, _paste_masked(gold, ImageChops.subtract(om, im)))
    glow = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((cx - 0.13 * big, cy - 0.13 * big, cx + 0.13 * big, cy + 0.13 * big),
                                 fill=JADE + (170,))
    glow = glow.filter(ImageFilter.GaussianBlur(0.05 * big))
    img = Image.alpha_composite(img, glow)
    draw = ImageDraw.Draw(img)
    star_outer = _stroke(size, 0.145, 2.6) * scale
    star_inner = _stroke(size, 0.036, 1.0) * scale
    _spark(draw, cx, cy, star_outer, star_inner, JADE_BRIGHT + (255,))
    if size >= 32:
        dot = 0.024 * big
        draw.ellipse((cx - dot, cy - dot, cx + dot, cy + dot), fill=(255, 255, 255, 240))
    return img.resize((size, size), Image.LANCZOS)


def _draw_hex_badge(size):
    """方案 N：六边形徽章，金环 + 玉色六角内芯。"""
    from PIL import ImageChops
    scale = SS
    big = size * scale
    img = _tile(size).resize((big, big), Image.LANCZOS)
    cx, cy = 0.5 * big, 0.5 * big

    def hex_mask(r):
        mask = Image.new('L', (big, big), 0)
        points = []
        for i in range(6):
            theta = -math.pi / 2 + i * math.pi / 3
            points.append((cx + math.cos(theta) * r, cy + math.sin(theta) * r))
        ImageDraw.Draw(mask).polygon(points, fill=255)
        return mask

    outer_r = 0.372 * big
    stroke = _stroke(size, 0.060 if size < 32 else 0.050, 2.2) * scale
    gold = Image.new('RGBA', (big, big), GOLD + (250,))
    img = Image.alpha_composite(img, _paste_masked(
        gold, ImageChops.subtract(hex_mask(outer_r), hex_mask(outer_r - stroke))))
    if size >= 24:
        jade = Image.new('RGBA', (big, big), JADE_BRIGHT + (225,))
        jade_stroke = _stroke(size, 0.034, 1.5) * scale
        jade_r = 0.205 * big
        img = Image.alpha_composite(img, _paste_masked(
            jade, ImageChops.subtract(hex_mask(jade_r), hex_mask(jade_r - jade_stroke))))
    glow = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((cx - 0.10 * big, cy - 0.10 * big, cx + 0.10 * big, cy + 0.10 * big),
                                 fill=JADE_BRIGHT + (170,))
    glow = glow.filter(ImageFilter.GaussianBlur(0.04 * big))
    img = Image.alpha_composite(img, glow)
    draw = ImageDraw.Draw(img)
    star_outer = _stroke(size, 0.088, 1.8) * scale
    star_inner = _stroke(size, 0.024, 0.8) * scale
    _spark(draw, cx, cy, star_outer, star_inner, (255, 255, 255, 245))
    return img.resize((size, size), Image.LANCZOS)


def _accent_dot(img, big, x, y, r):
    """玉色光点 + 白色芯，用于脉冲/折线端点。"""
    draw = ImageDraw.Draw(img)
    dot = r * big
    draw.ellipse((x * big - dot, y * big - dot, x * big + dot, y * big + dot),
                 fill=JADE_BRIGHT + (255,))
    core = dot * 0.42
    draw.ellipse((x * big - core, y * big - core, x * big + core, y * big + core),
                 fill=(255, 255, 255, 244))


def _draw_scope(size):
    """方案 O：示波器面板，金色网格 + 脉冲波形 + 玉色峰值。"""
    scale = SS
    big = size * scale
    img = _tile(size).resize((big, big), Image.LANCZOS)
    p0, p1 = (0.145, 0.185), (0.855, 0.815)
    panel = [p0[0] * big, p0[1] * big, p1[0] * big, p1[1] * big]
    radius = int(0.062 * big)
    fill = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(fill).rounded_rectangle(panel, radius=radius, fill=(12, 10, 6, 235))
    img = Image.alpha_composite(img, fill)
    border = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    bdraw = ImageDraw.Draw(border)
    bdraw.rounded_rectangle(panel, radius=radius, outline=GOLD + (245,),
                            width=max(1, int(round(_stroke(size, 0.024 if size < 32 else 0.019, 1.6) * scale))))
    if size >= 32:
        for i in (1, 2):
            y = (p0[1] + (p1[1] - p0[1]) * i / 3) * big
            x = (p0[0] + (p1[0] - p0[0]) * i / 3) * big
            bdraw.line([(p0[0] * big, y), (p1[0] * big, y)], fill=GOLD_DIM + (130,),
                       width=max(1, int(round(0.006 * big))))
            bdraw.line([(x, p0[1] * big), (x, p1[1] * big)], fill=GOLD_DIM + (130,),
                       width=max(1, int(round(0.006 * big))))
    img = Image.alpha_composite(img, border)
    points = [(0.190, 0.560), (0.270, 0.560), (0.330, 0.385), (0.395, 0.715),
              (0.470, 0.470), (0.540, 0.560), (0.640, 0.560), (0.700, 0.505),
              (0.810, 0.560)]
    width = _stroke(size, 0.055 if size < 32 else 0.042, 1.8) * scale
    trace = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(trace).line([(x * big, y * big) for x, y in points],
                               fill=GOLD_BRIGHT + (255,), width=int(round(width)), joint='curve')
    glow = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    gdraw = ImageDraw.Draw(glow)
    gdraw.ellipse((0.395 * big - 0.075 * big, 0.715 * big - 0.075 * big,
                   0.395 * big + 0.075 * big, 0.715 * big + 0.075 * big), fill=JADE + (190,))
    glow = glow.filter(ImageFilter.GaussianBlur(0.045 * big))
    img = Image.alpha_composite(img, glow)
    img = Image.alpha_composite(img, trace)
    _accent_dot(img, big, 0.395, 0.715, 0.040 if size >= 32 else 0.050)
    return img.resize((size, size), Image.LANCZOS)


def _draw_chart(size):
    """方案 P：上升折线（性能提升语义），玉色端点。"""
    scale = SS
    big = size * scale
    img = _tile(size).resize((big, big), Image.LANCZOS)
    base_y = 0.815
    baseline = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(baseline).line([(0.165 * big, base_y * big), (0.845 * big, base_y * big)],
                                  fill=GOLD_DIM + (190,), width=max(1, int(round(0.014 * big))))
    img = Image.alpha_composite(img, baseline)
    points = [(0.165, 0.700), (0.315, 0.615), (0.450, 0.655), (0.590, 0.500), (0.720, 0.418)]
    area = [(x * big, y * big) for x, y in points]
    area += [(0.720 * big, base_y * big), (0.165 * big, base_y * big)]
    fill = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(fill).polygon(area, fill=GOLD + (70,))
    img = Image.alpha_composite(img, fill)
    width = _stroke(size, 0.062 if size < 32 else 0.046, 2.2) * scale
    line = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(line).line([(x * big, y * big) for x, y in points],
                              fill=GOLD_BRIGHT + (255,), width=int(round(width)), joint='curve')
    img = Image.alpha_composite(img, line)
    glow = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((0.720 * big - 0.080 * big, 0.418 * big - 0.080 * big,
                                  0.720 * big + 0.080 * big, 0.418 * big + 0.080 * big),
                                 fill=JADE + (185,))
    glow = glow.filter(ImageFilter.GaussianBlur(0.045 * big))
    img = Image.alpha_composite(img, glow)
    _accent_dot(img, big, 0.720, 0.418, 0.042 if size >= 32 else 0.052)
    if size >= 32:
        # 起点小玉点，强调“起点→提升”。
        draw = ImageDraw.Draw(img)
        dot = 0.020 * big
        draw.ellipse((0.165 * big - dot, 0.700 * big - dot, 0.165 * big + dot, 0.700 * big + dot),
                     fill=JADE + (210,))
    return img.resize((size, size), Image.LANCZOS)


def _draw_ring_pulse(size):
    """方案 Q：利用率圆环 + 中央脉冲线。"""
    scale = SS
    big = size * scale
    img = _tile(size).resize((big, big), Image.LANCZOS)
    cx, cy = 0.5 * big, 0.5 * big
    r = 0.330 * big
    width = max(1, int(round(_stroke(size, 0.085 if size < 32 else 0.068, 2.4) * scale)))
    track = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(track).ellipse((cx - r, cy - r, cx + r, cy + r), outline=GOLD_DIM + (190,), width=width)
    img = Image.alpha_composite(img, track)
    prog = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(prog).arc((cx - r, cy - r, cx + r, cy + r), start=135, end=300,
                             fill=JADE_BRIGHT + (250,), width=width)
    img = Image.alpha_composite(img, prog)
    points = [(0.290, 0.500), (0.365, 0.500), (0.420, 0.390), (0.480, 0.585),
              (0.545, 0.455), (0.610, 0.500), (0.700, 0.500)]
    lw = _stroke(size, 0.045 if size < 32 else 0.034, 1.8) * scale
    pulse = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(pulse).line([(x * big, y * big) for x, y in points],
                               fill=GOLD_BRIGHT + (255,), width=int(round(lw)), joint='curve')
    img = Image.alpha_composite(img, pulse)
    _accent_dot(img, big, 0.545, 0.455, 0.036 if size >= 32 else 0.046)
    return img.resize((size, size), Image.LANCZOS)


def _draw_bars_pulse(size):
    """方案 R：柱状统计 + 顶部脉冲线。"""
    scale = SS
    big = size * scale
    img = _tile(size).resize((big, big), Image.LANCZOS)
    heights = [0.200, 0.300, 0.400, 0.520]
    colors = [GOLD_DIM + (255,), GOLD + (255,), GOLD_BRIGHT + (255,), JADE_BRIGHT + (255,)]
    left, bar_w, gap = 0.255, 0.120, 0.042
    base_y = 0.790
    for i, (h, color) in enumerate(zip(heights, colors)):
        x0 = (left + i * (bar_w + gap)) * big
        y0 = (base_y - h) * big
        ImageDraw.Draw(img).rounded_rectangle((x0, y0, x0 + bar_w * big, base_y * big),
                                              radius=int(0.040 * big), fill=color)
    points = [(0.190, 0.300), (0.300, 0.300), (0.360, 0.215), (0.425, 0.355),
              (0.495, 0.270), (0.570, 0.300), (0.810, 0.300)]
    lw = _stroke(size, 0.036 if size < 32 else 0.028, 1.6) * scale
    pulse = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(pulse).line([(x * big, y * big) for x, y in points],
                               fill=GOLD_BRIGHT + (235,), width=int(round(lw)), joint='curve')
    img = Image.alpha_composite(img, pulse)
    _accent_dot(img, big, 0.425, 0.355, 0.030 if size >= 32 else 0.040)
    return img.resize((size, size), Image.LANCZOS)


def _draw_gauge_zone(size):
    """方案 S：仪表盘 + 玉色提升区，强调“从中段进入高性能区”。"""
    scale = SS
    big = size * scale
    img = _tile(size).resize((big, big), Image.LANCZOS)
    cx, cy = 0.5 * big, 0.560 * big
    r = 0.330 * big
    width = max(1, int(round(_stroke(size, 0.080 if size < 32 else 0.062, 2.4) * scale)))
    arc = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(arc).arc((cx - r, cy - r, cx + r, cy + r), start=150, end=390,
                            fill=GOLD + (255,), width=width)
    img = Image.alpha_composite(img, arc)
    zone = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(zone).arc((cx - r, cy - r, cx + r, cy + r), start=295, end=390,
                             fill=JADE_BRIGHT + (255,), width=width)
    img = Image.alpha_composite(img, zone)
    if size >= 32:
        ticks = Image.new('RGBA', (big, big), (0, 0, 0, 0))
        tdraw = ImageDraw.Draw(ticks)
        for k in range(5):
            ang = math.radians(150 + k * 60)
            tdraw.line([(cx + math.cos(ang) * (r - width * 1.30),
                         cy + math.sin(ang) * (r - width * 1.30)),
                        (cx + math.cos(ang) * (r - width * 0.40),
                         cy + math.sin(ang) * (r - width * 0.40))],
                       fill=GOLD_DIM + (220,), width=max(1, int(round(0.013 * big))))
        img = Image.alpha_composite(img, ticks)
    ang = math.radians(-38)
    needle = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(needle).line([(cx, cy), (cx + math.cos(ang) * (r - width * 1.25),
                                            cy + math.sin(ang) * (r - width * 1.25))],
                                fill=JADE_BRIGHT + (255,),
                                width=max(1, int(round(_stroke(size, 0.050, 1.8) * scale))))
    img = Image.alpha_composite(img, needle)
    draw = ImageDraw.Draw(img)
    hub = 0.060 * big
    draw.ellipse((cx - hub, cy - hub, cx + hub, cy + hub), fill=GOLD + (255,))
    inner = 0.026 * big
    draw.ellipse((cx - inner, cy - inner, cx + inner, cy + inner), fill=(255, 255, 255, 235))
    return img.resize((size, size), Image.LANCZOS)


def _draw_dual_gauge(size):
    """方案 T：双表盘（源/输出帧率），金色弧 + 玉色指针。"""
    scale = SS
    big = size * scale
    img = _tile(size).resize((big, big), Image.LANCZOS)
    width = max(1, int(round(_stroke(size, 0.058 if size < 32 else 0.046, 2.0) * scale)))
    for cx_f, ang_deg in ((0.312, -55), (0.688, -32)):
        cx, cy, r = cx_f * big, 0.545 * big, 0.172 * big
        arc = Image.new('RGBA', (big, big), (0, 0, 0, 0))
        ImageDraw.Draw(arc).arc((cx - r, cy - r, cx + r, cy + r), start=145, end=395,
                                fill=GOLD + (250,), width=width)
        img = Image.alpha_composite(img, arc)
        ang = math.radians(ang_deg)
        needle = Image.new('RGBA', (big, big), (0, 0, 0, 0))
        ImageDraw.Draw(needle).line([(cx, cy), (cx + math.cos(ang) * r * 0.86,
                                                cy + math.sin(ang) * r * 0.86)],
                                    fill=JADE_BRIGHT + (255,),
                                    width=max(1, int(round(_stroke(size, 0.042, 1.6) * scale))))
        img = Image.alpha_composite(img, needle)
        draw = ImageDraw.Draw(img)
        hub = 0.048 * big
        draw.ellipse((cx - hub, cy - hub, cx + hub, cy + hub), fill=GOLD + (255,))
        inner = 0.020 * big
        draw.ellipse((cx - inner, cy - inner, cx + inner, cy + inner), fill=(255, 255, 255, 235))
    return img.resize((size, size), Image.LANCZOS)


def _draw_scope_balanced(size):
    """方案 O1：示波器·平衡版——波形居中、主峰在中心，网格更轻。"""
    scale = SS
    big = size * scale
    img = _tile(size).resize((big, big), Image.LANCZOS)
    p0, p1 = (0.140, 0.175), (0.860, 0.825)
    panel = [p0[0] * big, p0[1] * big, p1[0] * big, p1[1] * big]
    radius = int(0.075 * big)
    fill = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(fill).rounded_rectangle(panel, radius=radius, fill=(11, 9, 5, 240))
    img = Image.alpha_composite(img, fill)
    border = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    bdraw = ImageDraw.Draw(border)
    bdraw.rounded_rectangle(panel, radius=radius, outline=GOLD + (245,),
                            width=max(1, int(round(_stroke(size, 0.022 if size < 32 else 0.018, 1.6) * scale))))
    if size >= 32:
        # 细网格 + 中心基线，比 3x3 等分更克制。
        base = 0.5 * (p0[1] + p1[1]) * big
        bdraw.line([(p0[0] * big + 0.012 * big, base), (p1[0] * big - 0.012 * big, base)],
                   fill=GOLD_DIM + (150,), width=max(1, int(round(0.005 * big))))
        for fx in (1.0 / 3.0, 2.0 / 3.0):
            x = (p0[0] + (p1[0] - p0[0]) * fx) * big
            bdraw.line([(x, p0[1] * big + 0.012 * big), (x, p1[1] * big - 0.012 * big)],
                       fill=GOLD_DIM + (95,), width=max(1, int(round(0.005 * big))))
        for fy in (1.0 / 3.0, 2.0 / 3.0):
            y = (p0[1] + (p1[1] - p0[1]) * fy) * big
            bdraw.line([(p0[0] * big + 0.012 * big, y), (p1[0] * big - 0.012 * big, y)],
                       fill=GOLD_DIM + (95,), width=max(1, int(round(0.005 * big))))
    img = Image.alpha_composite(img, border)
    points = [(0.175, 0.585), (0.285, 0.585), (0.360, 0.445), (0.435, 0.705),
              (0.500, 0.330), (0.565, 0.705), (0.640, 0.445), (0.715, 0.585),
              (0.825, 0.585)]
    width = _stroke(size, 0.055 if size < 32 else 0.042, 1.8) * scale
    trace = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(trace).line([(x * big, y * big) for x, y in points],
                               fill=GOLD_BRIGHT + (255,), width=int(round(width)), joint='curve')
    glow = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((0.500 * big - 0.085 * big, 0.360 * big - 0.085 * big,
                                  0.500 * big + 0.085 * big, 0.360 * big + 0.085 * big),
                                 fill=JADE + (190,))
    glow = glow.filter(ImageFilter.GaussianBlur(0.045 * big))
    img = Image.alpha_composite(img, glow)
    img = Image.alpha_composite(img, trace)
    _accent_dot(img, big, 0.500, 0.360, 0.042 if size >= 32 else 0.052)
    return img.resize((size, size), Image.LANCZOS)


def _draw_scope_dual(size):
    """方案 O2：示波器·双迹线——金色主迹 + 玉色副迹 + 触发线。"""
    scale = SS
    big = size * scale
    img = _tile(size).resize((big, big), Image.LANCZOS)
    p0, p1 = (0.140, 0.175), (0.860, 0.825)
    panel = [p0[0] * big, p0[1] * big, p1[0] * big, p1[1] * big]
    radius = int(0.075 * big)
    fill = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(fill).rounded_rectangle(panel, radius=radius, fill=(10, 8, 5, 242))
    img = Image.alpha_composite(img, fill)
    border = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    bdraw = ImageDraw.Draw(border)
    bdraw.rounded_rectangle(panel, radius=radius, outline=GOLD + (245,),
                            width=max(1, int(round(_stroke(size, 0.022 if size < 32 else 0.018, 1.6) * scale))))
    if size >= 32:
        for k in (1, 2):
            y = (p0[1] + (p1[1] - p0[1]) * k / 3.0) * big
            bdraw.line([(p0[0] * big, y), (p1[0] * big, y)], fill=GOLD_DIM + (100,),
                       width=max(1, int(round(0.005 * big))))
        for k in (1, 2):
            x = (p0[0] + (p1[0] - p0[0]) * k / 3.0) * big
            bdraw.line([(x, p0[1] * big), (x, p1[1] * big)], fill=GOLD_DIM + (100,),
                       width=max(1, int(round(0.005 * big))))
        # 触发线（虚线感：短段）。
        tx = 0.500 * big
        for start in (0.20, 0.30, 0.40, 0.50, 0.60, 0.70, 0.80):
            bdraw.line([(tx, start * big), (tx, (start + 0.045) * big)],
                       fill=JADE + (110,), width=max(1, int(round(0.006 * big))))
    img = Image.alpha_composite(img, border)
    # 副迹线（玉色，整体略低、幅度小）。
    sub = [(0.175, 0.725), (0.300, 0.725), (0.370, 0.650), (0.435, 0.760),
           (0.500, 0.640), (0.565, 0.760), (0.630, 0.650), (0.700, 0.725),
           (0.825, 0.725)]
    lw_sub = _stroke(size, 0.032 if size < 32 else 0.024, 1.4) * scale
    layer = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(layer).line([(x * big, y * big) for x, y in sub],
                               fill=JADE + (130,), width=int(round(lw_sub)), joint='curve')
    img = Image.alpha_composite(img, layer)
    main = [(0.175, 0.505), (0.285, 0.505), (0.360, 0.380), (0.435, 0.625),
            (0.500, 0.300), (0.565, 0.625), (0.640, 0.380), (0.715, 0.505),
            (0.825, 0.505)]
    width = _stroke(size, 0.050 if size < 32 else 0.038, 1.8) * scale
    trace = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(trace).line([(x * big, y * big) for x, y in main],
                               fill=GOLD_BRIGHT + (255,), width=int(round(width)), joint='curve')
    glow = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((0.500 * big - 0.080 * big, 0.330 * big - 0.080 * big,
                                  0.500 * big + 0.080 * big, 0.330 * big + 0.080 * big),
                                 fill=JADE + (185,))
    glow = glow.filter(ImageFilter.GaussianBlur(0.045 * big))
    img = Image.alpha_composite(img, glow)
    img = Image.alpha_composite(img, trace)
    _accent_dot(img, big, 0.500, 0.330, 0.040 if size >= 32 else 0.050)
    return img.resize((size, size), Image.LANCZOS)


def _draw_gauge_round(size):
    """方案 S1：表盘·圆角量规——圆头弧线、细轨道、外侧刻度、指针配重。"""
    scale = SS
    big = size * scale
    img = _tile(size).resize((big, big), Image.LANCZOS)
    cx, cy = 0.5 * big, 0.565 * big
    r = 0.330 * big
    width = int(round(_stroke(size, 0.072 if size < 32 else 0.052, 2.4) * scale))
    half = width / 2.0
    track = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    tdraw = ImageDraw.Draw(track)
    box = (cx - r, cy - r, cx + r, cy + r)
    tdraw.arc(box, start=150, end=390, fill=GOLD + (235,), width=width)
    for ang_deg in (150, 390):
        ang = math.radians(ang_deg)
        px, py = cx + math.cos(ang) * r, cy + math.sin(ang) * r
        tdraw.ellipse((px - half, py - half, px + half, py + half), fill=GOLD + (235,))
    img = Image.alpha_composite(img, track)
    zone = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    zdraw = ImageDraw.Draw(zone)
    zdraw.arc(box, start=298, end=390, fill=JADE_BRIGHT + (255,), width=width)
    for ang_deg in (298, 390):
        ang = math.radians(ang_deg)
        px, py = cx + math.cos(ang) * r, cy + math.sin(ang) * r
        zdraw.ellipse((px - half, py - half, px + half, py + half), fill=JADE_BRIGHT + (255,))
    img = Image.alpha_composite(img, zone)
    if size >= 32:
        ticks = Image.new('RGBA', (big, big), (0, 0, 0, 0))
        tdraw2 = ImageDraw.Draw(ticks)
        for k in range(5):
            ang = math.radians(150 + k * 60)
            tdraw2.line([(cx + math.cos(ang) * (r + half * 0.55),
                         cy + math.sin(ang) * (r + half * 0.55)),
                        (cx + math.cos(ang) * (r + half * 1.55),
                         cy + math.sin(ang) * (r + half * 1.55))],
                        fill=GOLD_DIM + (210,), width=max(1, int(round(0.011 * big))))
        img = Image.alpha_composite(img, ticks)
    ang = math.radians(-42)
    needle = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ndraw = ImageDraw.Draw(needle)
    nx = cx + math.cos(ang) * (r - half * 1.4)
    ny = cy + math.sin(ang) * (r - half * 1.4)
    ndraw.line([(cx, cy), (nx, ny)], fill=JADE_BRIGHT + (255,),
               width=max(1, int(round(_stroke(size, 0.046, 1.8) * scale))))

    img = Image.alpha_composite(img, needle)
    draw = ImageDraw.Draw(img)
    hub = 0.052 * big
    draw.ellipse((cx - hub, cy - hub, cx + hub, cy + hub), fill=GOLD + (255,))
    inner = 0.022 * big
    draw.ellipse((cx - inner, cy - inner, cx + inner, cy + inner), fill=(255, 255, 255, 240))
    return img.resize((size, size), Image.LANCZOS)


def _draw_gauge_marker(size):
    """方案 S2：表盘·分区标记——玉区起点三角标记、外圈刻度、玉石指针。"""
    scale = SS
    big = size * scale
    img = _tile(size).resize((big, big), Image.LANCZOS)
    cx, cy = 0.5 * big, 0.565 * big
    r = 0.330 * big
    width = int(round(_stroke(size, 0.085 if size < 32 else 0.062, 2.4) * scale))
    box = (cx - r, cy - r, cx + r, cy + r)
    track = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(track).arc(box, start=150, end=390, fill=GOLD + (255,), width=width)
    img = Image.alpha_composite(img, track)
    zone = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(zone).arc(box, start=300, end=390, fill=JADE_BRIGHT + (255,), width=width)
    img = Image.alpha_composite(img, zone)
    if size >= 32:
        ticks = Image.new('RGBA', (big, big), (0, 0, 0, 0))
        tdraw = ImageDraw.Draw(ticks)
        for k in range(9):
            ang = math.radians(150 + k * 30)
            tdraw.line([(cx + math.cos(ang) * (r + width * 0.62),
                         cy + math.sin(ang) * (r + width * 0.62)),
                        (cx + math.cos(ang) * (r + width * 1.02),
                         cy + math.sin(ang) * (r + width * 1.02))],
                       fill=GOLD_DIM + (205,), width=max(1, int(round(0.010 * big))))
        img = Image.alpha_composite(img, ticks)
        marker = Image.new('RGBA', (big, big), (0, 0, 0, 0))
        ang = math.radians(300)
        mx, my = cx + math.cos(ang) * r, cy + math.sin(ang) * r
        dx, dy = -math.cos(ang), -math.sin(ang)
        px, py = -dy, dx
        length, spread = 0.055 * big, 0.030 * big
        mdraw = ImageDraw.Draw(marker)
        mdraw.polygon([(mx + dx * length, my + dy * length),
                       (mx + px * spread, my + py * spread),
                       (mx - px * spread, my - py * spread)], fill=GOLD_BRIGHT + (255,))
        img = Image.alpha_composite(img, marker)
    ang = math.radians(-50)
    needle = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ndraw = ImageDraw.Draw(needle)
    nx = cx + math.cos(ang) * (r - width * 1.15)
    ny = cy + math.sin(ang) * (r - width * 1.15)
    ndraw.line([(cx, cy), (nx, ny)], fill=GOLD_BRIGHT + (255,),
               width=max(1, int(round(_stroke(size, 0.050, 2.0) * scale))))
    tip = 0.105
    ndraw.line([(cx + math.cos(ang) * (r - width * 1.15) * (1 - tip),
                 cy + math.sin(ang) * (r - width * 1.15) * (1 - tip)), (nx, ny)],
               fill=JADE_BRIGHT + (255,),
               width=max(1, int(round(_stroke(size, 0.050, 2.0) * scale))))
    img = Image.alpha_composite(img, needle)
    draw = ImageDraw.Draw(img)
    hub = 0.056 * big
    draw.ellipse((cx - hub, cy - hub, cx + hub, cy + hub), fill=GOLD + (255,))
    inner = 0.024 * big
    draw.ellipse((cx - inner, cy - inner, cx + inner, cy + inner), fill=JADE + (255,))
    return img.resize((size, size), Image.LANCZOS)


VARIANTS = {
    'a-frame-spark': _draw_frame_spark,
    'b-zoom-stack': _draw_zoom_stack,
    'c-bars': _draw_bars,
    'd-diamond': _draw_diamond,
}

# 第二轮候选：保持菱形语言，字形整体放大（顶点更靠外、星芒更大）。
ROUND2 = {
    'e-diamond-big': _draw_diamond_big,
    'f-jade-solid': _draw_jade_solid,
    'g-nested-rings': _draw_nested_rings,
    'h-gem-spark': _draw_gem_spark,
}


def _preview(outdir):
    os.makedirs(outdir, exist_ok=True)
    for name, fn in VARIANTS.items():
        big = fn(256)
        big.save(os.path.join(outdir, name + '-256.png'))
        small = fn(32)
        small.resize((128, 128), Image.NEAREST).save(os.path.join(outdir, name + '-32x4.png'))
        tiny = fn(16)
        tiny.resize((128, 128), Image.NEAREST).save(os.path.join(outdir, name + '-16x8.png'))
    # 汇总接触表：每行一个方案，列 256 / 32 / 16。
    sheet = Image.new('RGBA', (256 + 24 + 128 + 24 + 128 + 24, 256 * len(VARIANTS)), (24, 24, 24, 255))
    for row, name in enumerate(VARIANTS):
        y = row * 256
        fn = VARIANTS[name]
        sheet.alpha_composite(fn(256), (0, y))
        sheet.alpha_composite(fn(32).resize((128, 128), Image.NEAREST), (280, y + 64))
        sheet.alpha_composite(fn(16).resize((128, 128), Image.NEAREST), (432, y + 64))
    sheet.save(os.path.join(outdir, 'contact-sheet.png'))
    print('preview written to', outdir)


# 第三轮候选：换风格（字标 / 齿轮 / 脉冲 / 仪表 / 护盾 / 六边形徽章）。
ROUND3 = {
    'i-monogram': _draw_monogram,
    'j-gear': _draw_gear,
    'k-pulse': _draw_pulse,
    'l-gauge': _draw_gauge,
    'm-shield': _draw_shield,
    'n-hex-badge': _draw_hex_badge,
}


# 第四轮候选：仪表/监控语言（示波器、折线、圆环、柱状+脉冲、分区表盘、双表盘）。
ROUND4 = {
    'o-scope': _draw_scope,
    'p-chart': _draw_chart,
    'q-ring-pulse': _draw_ring_pulse,
    'r-bars-pulse': _draw_bars_pulse,
    's-gauge-zone': _draw_gauge_zone,
    't-dual-gauge': _draw_dual_gauge,
}


# 第五轮候选：O（示波器）与 S（分区表盘）的精修变体。
ROUND5 = {
    'o1-scope-balance': _draw_scope_balanced,
    'o2-scope-dual': _draw_scope_dual,
    's1-gauge-round': _draw_gauge_round,
    's2-gauge-marker': _draw_gauge_marker,
}


def _preview_round2(outdir):
    """第二轮候选预览：256 大图 + 64/48/32/24/16 实尺寸列 + 任务栏深/浅底对照。"""
    os.makedirs(outdir, exist_ok=True)
    cell = 88
    sheet = Image.new('RGBA', (256 + 16 + 5 * cell + 16, 256 * len(ROUND2)), (24, 24, 24, 255))
    for row, (name, fn) in enumerate(ROUND2.items()):
        y = row * 256
        sheet.alpha_composite(fn(256), (0, y))
        for col, size in enumerate((64, 48, 32, 24, 16)):
            icon = fn(size)
            sheet.alpha_composite(icon, (256 + 16 + col * cell + (cell - size) // 2,
                                         y + (256 - size) // 2))
        fn(256).save(os.path.join(outdir, name + '-256.png'))
        for bg_name, bg in (('dark', (32, 32, 32, 255)), ('light', (243, 243, 243, 255))):
            strip = Image.new('RGBA', (420, 140), bg)
            for i, size in enumerate((16, 24, 32, 48, 64)):
                strip.alpha_composite(fn(size), (12 + i * 78 + (64 - size) // 2, (140 - size) // 2))
            strip.save(os.path.join(outdir, name + '-' + bg_name + '.png'))
    sheet.save(os.path.join(outdir, 'contact-sheet-round2.png'))
    print('preview round2 written to', outdir)


def _preview_round3(outdir):
    """第三轮（换风格）候选预览：256 大图 + 实尺寸列 + 任务栏深/浅底对照。"""
    os.makedirs(outdir, exist_ok=True)
    cell = 88
    sheet = Image.new('RGBA', (256 + 16 + 5 * cell + 16, 256 * len(ROUND3)), (24, 24, 24, 255))
    for row, (name, fn) in enumerate(ROUND3.items()):
        y = row * 256
        sheet.alpha_composite(fn(256), (0, y))
        for col, size in enumerate((64, 48, 32, 24, 16)):
            icon = fn(size)
            sheet.alpha_composite(icon, (256 + 16 + col * cell + (cell - size) // 2,
                                         y + (256 - size) // 2))
        fn(256).save(os.path.join(outdir, name + '-256.png'))
        for bg_name, bg in (('dark', (32, 32, 32, 255)), ('light', (243, 243, 243, 255))):
            strip = Image.new('RGBA', (420, 140), bg)
            for i, size in enumerate((16, 24, 32, 48, 64)):
                strip.alpha_composite(fn(size), (12 + i * 78 + (64 - size) // 2, (140 - size) // 2))
            strip.save(os.path.join(outdir, name + '-' + bg_name + '.png'))
    sheet.save(os.path.join(outdir, 'contact-sheet-round3.png'))
    print('preview round3 written to', outdir)


def _preview_round5(outdir):
    """第五轮（O/S 精修）候选预览。"""
    os.makedirs(outdir, exist_ok=True)
    cell = 88
    sheet = Image.new('RGBA', (256 + 16 + 5 * cell + 16, 256 * len(ROUND5)), (24, 24, 24, 255))
    for row, (name, fn) in enumerate(ROUND5.items()):
        y = row * 256
        sheet.alpha_composite(fn(256), (0, y))
        for col, size in enumerate((64, 48, 32, 24, 16)):
            icon = fn(size)
            sheet.alpha_composite(icon, (256 + 16 + col * cell + (cell - size) // 2,
                                         y + (256 - size) // 2))
        fn(256).save(os.path.join(outdir, name + '-256.png'))
        for bg_name, bg in (('dark', (32, 32, 32, 255)), ('light', (243, 243, 243, 255))):
            strip = Image.new('RGBA', (420, 140), bg)
            for i, size in enumerate((16, 24, 32, 48, 64)):
                strip.alpha_composite(fn(size), (12 + i * 78 + (64 - size) // 2, (140 - size) // 2))
            strip.save(os.path.join(outdir, name + '-' + bg_name + '.png'))
    sheet.save(os.path.join(outdir, 'contact-sheet-round5.png'))
    print('preview round5 written to', outdir)


def _preview_round4(outdir):
    """第四轮（仪表/监控风格）候选预览。"""
    os.makedirs(outdir, exist_ok=True)
    cell = 88
    sheet = Image.new('RGBA', (256 + 16 + 5 * cell + 16, 256 * len(ROUND4)), (24, 24, 24, 255))
    for row, (name, fn) in enumerate(ROUND4.items()):
        y = row * 256
        sheet.alpha_composite(fn(256), (0, y))
        for col, size in enumerate((64, 48, 32, 24, 16)):
            icon = fn(size)
            sheet.alpha_composite(icon, (256 + 16 + col * cell + (cell - size) // 2,
                                         y + (256 - size) // 2))
        fn(256).save(os.path.join(outdir, name + '-256.png'))
        for bg_name, bg in (('dark', (32, 32, 32, 255)), ('light', (243, 243, 243, 255))):
            strip = Image.new('RGBA', (420, 140), bg)
            for i, size in enumerate((16, 24, 32, 48, 64)):
                strip.alpha_composite(fn(size), (12 + i * 78 + (64 - size) // 2, (140 - size) // 2))
            strip.save(os.path.join(outdir, name + '-' + bg_name + '.png'))
    sheet.save(os.path.join(outdir, 'contact-sheet-round4.png'))
    print('preview round4 written to', outdir)


def _bmp_entry(size):
    """把 RGBA 图编码为 ICO 内嵌的 32 位 BMP（DIB + AND 掩码）。"""
    img = VARIANTS_FINAL(size)
    pixels = img.load()
    rows = []
    for y in range(size - 1, -1, -1):
        row = bytearray()
        for x in range(size):
            r, g, b, a = pixels[x, y]
            row += bytes((b, g, r, a))
        rows.append(bytes(row))
    xor = b''.join(rows)
    mask_row = (size + 31) // 32 * 4
    and_mask = b'\x00' * (mask_row * size)
    header = struct.pack('<IiiHHIIiiII', 40, size, size * 2, 1, 32, 0, len(xor) + len(and_mask), 0, 0, 0, 0)
    return header + xor + and_mask


def _png_entry(size):
    import io
    buffer = io.BytesIO()
    VARIANTS_FINAL(size).save(buffer, format='PNG')
    return buffer.getvalue()


def _build(path):
    sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
    entries = []
    for size in sizes:
        data = _png_entry(size) if size >= 128 else _bmp_entry(size)
        entries.append((size, data))
    header = struct.pack('<HHH', 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    directory = bytearray()
    payload = bytearray()
    for size, data in entries:
        dimension = 0 if size >= 256 else size
        directory += struct.pack('<BBBBHHII', dimension, dimension, 0, 0, 1, 32, len(data), offset)
        payload += data
        offset += len(data)
    with open(path, 'wb') as handle:
        handle.write(header + directory + payload)
    print('icon written to', path, 'bytes', 6 + 16 * len(entries) + len(payload))


VARIANTS_FINAL = _draw_gauge_marker

if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == 'preview':
        _preview(sys.argv[2])
    elif len(sys.argv) >= 3 and sys.argv[1] == 'preview2':
        _preview_round2(sys.argv[2])
    elif len(sys.argv) >= 3 and sys.argv[1] == 'preview5':
        _preview_round5(sys.argv[2])
    elif len(sys.argv) >= 3 and sys.argv[1] == 'preview4':
        _preview_round4(sys.argv[2])
    elif len(sys.argv) >= 3 and sys.argv[1] == 'preview3':
        _preview_round3(sys.argv[2])
    elif len(sys.argv) >= 3 and sys.argv[1] == 'build':
        _build(sys.argv[2])
    else:
        print(__doc__)
        sys.exit(2)
