#!/usr/bin/env python3
"""Generate TamaTac background decor (Decor.h + PNG previews).

Pixel-art scenery as tiny assets:
  - A8 alpha silhouettes (sun, moon, 2 clouds) shown via lv_image with
    recolor + faded opa so they sit *behind* the UI
  - 32x16 RGB565 grass tiles (day/night), horizontally tileable ground strip

Art is procedural (circles/ellipses/waves with soft alpha falloff), so no
input files are needed. Also renders composed 480x270 day/night scene
previews (needs Pillow, same as sprite2c.py).

Usage:
    python gen_bgdecor.py   # writes ../../Apps/TamaTac/main/Source/Decor.h
                            #   and ../../Apps/TamaTac/decor/*.png
"""
import math
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent
OUT_H = ROOT / "../../Apps/TamaTac/main/Source/Decor.h"
OUT_IMG = ROOT / "../../Apps/TamaTac/decor"

def clamp255(v):
    return max(0, min(255, int(v)))


def disc(w, h, cx, cy, r):
    """Soft-edged disc alpha map."""
    out = []
    for y in range(h):
        for x in range(w):
            d = math.hypot(x - cx, y - cy)
            out.append(clamp255((r + 0.6 - d) * 160))
    return out


def sun():
    w = h = 24
    a = disc(w, h, 11.5, 11.5, 6.5)
    # 8 stubby rays (2px thick bars)
    for k in range(8):
        ang = math.pi / 4 * k
        horiz = (k % 2 == 0)
        for t in range(8, 12):
            x = int(round(11.5 + math.cos(ang) * t))
            y = int(round(11.5 + math.sin(ang) * t))
            pts = [(x, y), (x + 1, y)] if horiz else [(x, y), (x, y + 1)]
            for (xx, yy) in pts:
                if 0 <= xx < w and 0 <= yy < h:
                    a[yy * w + xx] = max(a[yy * w + xx], 220)
    return w, h, a


def moon():
    return 20, 20, disc(20, 20, 9.5, 9.5, 8.0)


def cloud(w, h, blobs):
    out = [0] * (w * h)
    for y in range(h):
        for x in range(w):
            best = 0.0
            for (cx, cy, rx, ry) in blobs:
                d = math.hypot((x - cx) / rx, (y - cy) / ry)
                best = max(best, (1.15 - d) * 200)
            out[y * w + x] = clamp255(best)
    return w, h, out


def cloud_big():
    w, h = 40, 20
    return cloud(w, h, [(12, 13, 11, 6.5), (21, 10, 12, 8),
                        (30, 13, 9, 5.5)])


def cloud_small():
    w, h = 28, 14
    return cloud(w, h, [(9, 9, 8, 4.5), (15, 7, 8.5, 5.5), (21, 9, 6.5, 4)])


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def grass(night):
    """32x16 tileable ground tile. Wave period 8 divides 32: seamless."""
    w, h = 32, 16
    if night:
        top, mid, dirt, speck = (16, 26, 60), (12, 20, 48), (26, 20, 40), (16, 14, 30)
    else:
        top, mid, dirt, speck = (46, 140, 70), (34, 112, 56), (122, 88, 52), (96, 68, 40)
    out = []
    for y in range(h):
        for x in range(w):
            edge = 3 + ((x // 4) % 2) * 2  # blocky wave, period 8
            blade = (x * 7 + 3) % 8 == 0 and y < edge + 3
            if y < edge:
                c = top if (x + y) % 2 == 0 else mid
            elif y == edge or blade:
                c = top
            elif (x * 31 + y * 57) % 11 == 0:
                c = speck
            else:
                c = dirt if (x + y) % 2 == 0 else speck
            out.append(rgb565(*c))
    return w, h, out


PIECES_A8 = {"decorSun": sun(), "decorMoon": moon(),
             "cloudBig": cloud_big(), "cloudSmall": cloud_small()}
GRASS = {"grassDay": grass(False), "grassNight": grass(True)}


def fmt_bytes(name, data):
    rows = []
    w = PIECES_A8[name][0]
    for y in range(len(data) // w):
        rows.append(", ".join(f"0x{b:02X}" for b in data[y * w:(y + 1) * w]))
    body = ",\n    ".join(rows)
    return (f"// A8 alpha silhouette ({w}x{len(data) // w})\n"
            f"constexpr uint8_t {name}[{len(data)}] = {{\n    {body}\n}};")


def fmt_rgb(name, w, h, data):
    rows = [", ".join(f"0x{v:04X}" for v in data[y * w:(y + 1) * w])
            for y in range(h)]
    body = ",\n    ".join(rows)
    return (f"// {w}x{h} RGB565 tileable ground tile\n"
            f"constexpr uint16_t {name}[{len(data)}] = {{\n    {body}\n}};")


def dsc_a8(name, arr, w, h):
    return (f"inline const lv_image_dsc_t {name}Dsc = {{\n"
            f"    .header = {{ .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8,\n"
            f"                .flags = 0, .w = {w}, .h = {h}, .stride = {w},\n"
            f"                .reserved_2 = 0 }},\n"
            f"    .data_size = {w * h},\n"
            f"    .data = {arr},\n"
            f"    .reserved = nullptr,\n"
            f"    .reserved_2 = nullptr,\n"
            f"}};")


def dsc_rgb(name, arr, w, h):
    return (f"inline const lv_image_dsc_t {name}Dsc = {{\n"
            f"    .header = {{ .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,\n"
            f"                .flags = 0, .w = {w}, .h = {h}, .stride = {w * 2},\n"
            f"                .reserved_2 = 0 }},\n"
            f"    .data_size = {w * h * 2},\n"
            f"    .data = reinterpret_cast<const uint8_t*>({arr}),\n"
            f"    .reserved = nullptr,\n"
            f"    .reserved_2 = nullptr,\n"
            f"}};")


parts, dscs = [], []
for name, (w, h, data) in PIECES_A8.items():
    parts.append(fmt_bytes(name, data))
    dscs.append(dsc_a8(name, name, w, h))
for name, (w, h, data) in GRASS.items():
    parts.append(fmt_rgb(name, w, h, data))
    dscs.append(dsc_rgb(name, name, w, h))

header = f"""/**
 * @file Decor.h
 * @brief Background scenery for TamaTac (AUTO-GENERATED by gen_bgdecor.py).
 *
 * A8 alpha silhouettes (sun/moon/clouds, recolored+faded at runtime) and
 * tileable RGB565 grass tiles. Do not hand-edit: 
 * tweak Scripts/sprite_tools/gen_bgdecor.py and re-run.
 */
#pragma once

#include <lvgl.h>
#include <cstdint>

{chr(10).join(parts)}

{chr(10).join(dscs)}
"""
OUT_H.write_text(header)

# --- PNG previews (composed scenes, like spritedata2png.py output) ---
try:
    from PIL import Image
except ImportError:
    print(f"wrote {OUT_H} (Pillow missing: previews skipped)")
    raise SystemExit

OUT_IMG.mkdir(parents=True, exist_ok=True)

# Preview images
GRAIN = {
    False: ((71, 96, 184), (22, 31, 70), (66, 86, 142)),
    True: ((12, 18, 39), (9, 11, 34), (225, 225, 245)),
}


def grain_tile(night):
    base, speck, accent = GRAIN[night]
    img = Image.new("RGB", (32, 32), base)
    px = img.load()
    return img


def paste_a8(base, name, color, opacity, pos):
    w, h, data = PIECES_A8[name]
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    px = layer.load()
    for y in range(h):
        for x in range(w):
            a = data[y * w + x]
            px[x, y] = (*color, int(a * opacity / 255))
    base.alpha_composite(layer, pos)


def paste_grass(base, night, y0):
    w, h, data = GRASS["grassNight" if night else "grassDay"]
    tile = Image.new("RGB", (w, h))
    px = tile.load()
    for y in range(h):
        for x in range(w):
            v = data[y * w + x]
            px[x, y] = (((v >> 11) & 31) * 255 // 31,
                        ((v >> 5) & 63) * 255 // 63,
                        (v & 31) * 255 // 31)
    for x in range(0, base.width, w):
        base.paste(tile, (x, y0))


# Recolor + fade matching the firmware styles
SUN, MOON, CLOUD = (255, 217, 74), (232, 232, 245), (255, 255, 255)
FADE = 110  # ~43% opa

for night in (False, True):
    scene = Image.new("RGBA", (480, 270), (0, 0, 0, 255))
    g = grain_tile(night)
    for ty in range(0, 270, 32):
        for tx in range(0, 480, 32):
            scene.paste(g, (tx, ty))
    if night:
        paste_a8(scene, "decorMoon", MOON, 200, (410, 22))
    else:
        paste_a8(scene, "decorSun", SUN, 200, (410, 22))
        paste_a8(scene, "cloudBig", CLOUD, FADE, (24, 32))
        paste_a8(scene, "cloudSmall", CLOUD, FADE, (192, 16))
        paste_a8(scene, "cloudSmall", CLOUD, FADE - 20, (346, 44))
    paste_grass(scene, night, 270 - 16)
    scene.convert("RGB").save(OUT_IMG / ("scene_night.png" if night else "scene_day.png"))

print(f"wrote {OUT_H} + previews in {OUT_IMG}/")
