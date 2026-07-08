#!/usr/bin/env python3
"""Generate Flurry branding assets: Home Menu icons (48x48) + banners (256x128),
plus UU store icons. Snow/ice theme."""
import math
from PIL import Image, ImageDraw, ImageFont, ImageFilter

FONT_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

# Palette
ICE_LIGHT = (198, 236, 255)
ICE = (120, 200, 255)
ICE_DEEP = (60, 140, 230)
BG_TOP = (10, 22, 48)
BG_BOT = (4, 10, 26)
HIMEM_TOP = (54, 16, 24)
HIMEM_BOT = (20, 6, 12)
NIGHT_TOP = (30, 14, 52)
NIGHT_BOT = (12, 6, 26)


def vgradient(w, h, top, bot):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / (h - 1)
        px_row = tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3))
        for x in range(w):
            px[x, y] = px_row
    return img


def radial_glow(w, h, cx, cy, radius, color, strength=1.0):
    glow = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(glow)
    steps = 24
    for i in range(steps, 0, -1):
        r = radius * i / steps
        a = int(255 * strength * (1 - i / steps) ** 1.6)
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=a)
    col = Image.new("RGB", (w, h), color)
    base = Image.new("RGB", (w, h), (0, 0, 0))
    return Image.composite(col, base, glow)


def draw_snowflake(draw, cx, cy, r, color, width, arms=6, glow=None):
    for k in range(arms):
        ang = math.pi * 2 * k / arms
        ex, ey = cx + r * math.cos(ang), cy + r * math.sin(ang)
        draw.line([cx, cy, ex, ey], fill=color, width=width)
        # branches
        for frac, blen in ((0.55, 0.28), (0.78, 0.20)):
            bx, by = cx + r * frac * math.cos(ang), cy + r * frac * math.sin(ang)
            for s in (-1, 1):
                a2 = ang + s * math.radians(45)
                draw.line([bx, by, bx + r * blen * math.cos(a2), by + r * blen * math.sin(a2)],
                          fill=color, width=max(1, width - 1))
    draw.ellipse([cx - width, cy - width, cx + width, cy + width], fill=color)


def add_glow_layer(base, drawfn, color, blur=6):
    layer = Image.new("RGB", base.size, (0, 0, 0))
    d = ImageDraw.Draw(layer)
    drawfn(d)
    layer = layer.filter(ImageFilter.GaussianBlur(blur))
    tint = Image.new("RGB", base.size, color)
    lum = layer.convert("L")
    return Image.composite(tint, base, lum.point(lambda v: min(255, int(v * 2.2))))


def make_icon(path, accent_top, accent_bot, badge=None, badge_color=None):
    S = 48
    up = 8  # supersample
    W = S * up
    img = vgradient(W, W, accent_top, accent_bot)
    glow = radial_glow(W, W, W * 0.5, W * 0.46, W * 0.5, (30, 70, 130), 0.8)
    img = Image.blend(img, glow, 0.5)

    def flake(d):
        draw_snowflake(d, W * 0.5, W * 0.46, W * 0.34, ICE_LIGHT, up * 2)
    img = add_glow_layer(img, flake, ICE, blur=up * 3)
    d = ImageDraw.Draw(img)
    draw_snowflake(d, W * 0.5, W * 0.46, W * 0.34, ICE_LIGHT, max(2, up * 2 - 2))

    if badge:
        f = ImageFont.truetype(FONT_BOLD, int(W * 0.20))
        bb = d.textbbox((0, 0), badge, font=f)
        tw = bb[2] - bb[0]
        d.rectangle([0, W - int(W * 0.24), W, W], fill=badge_color)
        d.text(((W - tw) / 2 - bb[0], W - int(W * 0.245)), badge, font=f, fill=(255, 255, 255))

    img.resize((S, S), Image.LANCZOS).convert("RGB").save(path)


def make_banner(path, top, bot, title="Flurry", subtitle=None, accent=ICE):
    W, H = 256, 128
    up = 4
    img = vgradient(W * up, H * up, top, bot)
    glow = radial_glow(W * up, H * up, W * up * 0.30, H * up * 0.5, W * up * 0.5, (30, 80, 150), 0.7)
    img = Image.blend(img, glow, 0.5)

    # scattered snowflakes
    def flakes(d):
        pts = [(0.10, 0.28, 0.09), (0.18, 0.72, 0.06), (0.30, 0.20, 0.05),
               (0.30, 0.80, 0.07), (0.42, 0.50, 0.05)]
        for fx, fy, fr in pts:
            draw_snowflake(d, W * up * fx, H * up * fy, W * up * fr, ICE_LIGHT, up * 2)
    img = add_glow_layer(img, flakes, accent, blur=up * 4)
    d = ImageDraw.Draw(img)

    def flakes2(dd):
        pts = [(0.10, 0.28, 0.09), (0.18, 0.72, 0.06), (0.30, 0.20, 0.05),
               (0.30, 0.80, 0.07), (0.42, 0.50, 0.05)]
        for fx, fy, fr in pts:
            draw_snowflake(dd, W * up * fx, H * up * fy, W * up * fr, ICE_LIGHT, max(1, up * 2 - 1))
    flakes2(d)

    # wordmark: auto-fit into the right ~62% of the banner
    region_x = W * up * 0.40
    region_w = W * up * 0.56
    size = int(H * up * 0.46)
    while size > 8:
        f = ImageFont.truetype(FONT_BOLD, size)
        bb = d.textbbox((0, 0), title, font=f)
        if (bb[2] - bb[0]) <= region_w:
            break
        size -= 2
    tw, th = bb[2] - bb[0], bb[3] - bb[1]
    tx = region_x + (region_w - tw) / 2 - bb[0]
    block_h = th + (H * up * 0.20 if subtitle else 0)
    ty = (H * up - block_h) / 2 - bb[1]
    d.text((tx + up * 2, ty + up * 2), title, font=f, fill=(0, 0, 0))
    d.text((tx, ty), title, font=f, fill=ICE_LIGHT)
    if subtitle:
        fs = ImageFont.truetype(FONT_BOLD, int(H * up * 0.17))
        sb = d.textbbox((0, 0), subtitle, font=fs)
        sx = region_x + (region_w - (sb[2] - sb[0])) / 2 - sb[0]
        d.text((sx, ty + th + up * 5), subtitle, font=fs, fill=accent)

    img.resize((W, H), Image.LANCZOS).convert("RGBA").save(path)


if __name__ == "__main__":
    import os
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    # Home Menu loader assets (consumed by bannertool via HzLoad/Makefile)
    ld = os.path.join(root, "HzLoad", "assets")
    make_icon(os.path.join(ld, "logo.png"), BG_TOP, BG_BOT)
    make_icon(os.path.join(ld, "logo_HIMEM.png"), HIMEM_TOP, HIMEM_BOT, badge="HIMEM", badge_color=(150, 40, 40))
    make_banner(os.path.join(ld, "banner.png"), BG_TOP, BG_BOT, "Flurry")
    make_banner(os.path.join(ld, "banner_HIMEM.png"), HIMEM_TOP, HIMEM_BOT, "Flurry", "HIMEM", accent=(255, 150, 150))

    # Universal-Updater store icons (packed into a .t3x spritesheet at build time).
    # Order here defines icon_index: 0 = stable, 1 = nightly.
    st = os.path.join(root, "assets", "store")
    os.makedirs(st, exist_ok=True)
    make_icon(os.path.join(st, "icon0_stable.png"), BG_TOP, BG_BOT)
    make_icon(os.path.join(st, "icon1_nightly.png"), NIGHT_TOP, NIGHT_BOT, badge="DEV", badge_color=(90, 50, 150))
    print("Flurry assets regenerated.")
