#!/usr/bin/env python3
"""Generate placeholder 24x24 RGB565 sprite data for TamaTac.

Each sprite is a colored version of the original monochrome design,
upscaled from 16x16 to 24x24 with 2 animation frames (slight bounce).
"""

import math

T = 0xF81F  # Transparent (magenta)

def rgb565(r, g, b):
    """Convert 8-bit RGB to RGB565, avoiding transparent color."""
    val = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    if val == T:
        val = 0xF81E
    return val

# Color palette
WHITE    = rgb565(255, 255, 255)
CREAM    = rgb565(255, 240, 220)
LGRAY    = rgb565(200, 200, 200)
GRAY     = rgb565(150, 150, 150)
DGRAY    = rgb565(80, 80, 80)
BLACK    = rgb565(20, 20, 20)
RED      = rgb565(255, 60, 60)
DRED     = rgb565(180, 30, 30)
GREEN    = rgb565(60, 200, 60)
BLUE     = rgb565(80, 120, 255)
LBLUE    = rgb565(150, 200, 255)
YELLOW   = rgb565(255, 220, 50)
ORANGE   = rgb565(255, 160, 40)
PINK     = rgb565(255, 150, 180)
LPINK    = rgb565(255, 200, 220)
PURPLE   = rgb565(180, 100, 255)
LPURPLE  = rgb565(220, 180, 255)
CYAN     = rgb565(80, 220, 220)
BROWN    = rgb565(160, 100, 40)
LBROWN   = rgb565(200, 140, 80)
TEAL     = rgb565(50, 180, 160)

# Eye patterns (2x2 blocks)
def draw_eyes(grid, cx, cy, color=BLACK, highlight=WHITE):
    """Draw simple 2x2 eyes at left and right positions."""
    lx, rx = cx - 4, cx + 3
    ey = cy - 2
    for dy in range(2):
        for dx in range(2):
            grid[ey + dy][lx + dx] = color
            grid[ey + dy][rx + dx] = color
    # Highlight
    grid[ey][lx] = highlight
    grid[ey][rx] = highlight

def draw_mouth_smile(grid, cx, cy):
    """Draw a small smile."""
    y = cy + 2
    for dx in range(-2, 3):
        grid[y][cx + dx] = BLACK
    grid[y - 1][cx - 3] = BLACK
    grid[y - 1][cx + 3] = BLACK

def draw_mouth_frown(grid, cx, cy):
    """Draw a small frown."""
    y = cy + 3
    for dx in range(-2, 3):
        grid[y][cx + dx] = BLACK
    grid[y + 1][cx - 3] = BLACK
    grid[y + 1][cx + 3] = BLACK

def draw_mouth_open(grid, cx, cy):
    """Draw an open mouth (eating/surprised)."""
    y = cy + 2
    for dy in range(3):
        for dx in range(-2, 3):
            grid[y + dy][cx + dx] = BLACK
    # Inner mouth
    for dy in range(1, 2):
        for dx in range(-1, 2):
            grid[y + dy][cx + dx] = RED

def draw_x_eyes(grid, cx, cy):
    """Draw X-shaped eyes (sick)."""
    lx, rx = cx - 5, cx + 2
    ey = cy - 2
    for i in range(3):
        grid[ey + i][lx + i] = BLACK
        grid[ey + i][lx + 2 - i] = BLACK
        grid[ey + i][rx + i] = BLACK
        grid[ey + i][rx + 2 - i] = BLACK

def draw_closed_eyes(grid, cx, cy):
    """Draw closed eyes (sleeping)."""
    lx, rx = cx - 5, cx + 2
    ey = cy - 1
    for dx in range(3):
        grid[ey][lx + dx] = BLACK
        grid[ey][rx + dx] = BLACK

def make_oval(w, h, cx, cy, radius_x, radius_y, body_color, outline_color):
    """Create an oval shape with outline."""
    grid = [[T] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            dx = (x - cx) / radius_x
            dy = (y - cy) / radius_y
            dist = dx * dx + dy * dy
            if dist <= 0.85:
                grid[y][x] = body_color
            elif dist <= 1.0:
                grid[y][x] = outline_color
    return grid

def flatten(grid):
    """Flatten 2D grid to 1D array."""
    return [pixel for row in grid for pixel in row]

def shift_down(grid, pixels):
    """Shift grid contents down by N pixels (for bounce animation)."""
    h = len(grid)
    w = len(grid[0])
    new_grid = [[T] * w for _ in range(h)]
    for y in range(h - pixels):
        for x in range(w):
            new_grid[y + pixels][x] = grid[y][x]
    return new_grid

# ── Sprite Generators ─────────────────────────────────────────────────────────

def make_egg():
    """Egg: cream oval with cracks."""
    frames = []
    for bounce in [0, 1]:
        g = make_oval(24, 24, 12, 12, 8, 10, CREAM, LBROWN)
        # Crack pattern
        for x, y in [(10, 5), (11, 6), (12, 5), (13, 6), (14, 5)]:
            g[y + bounce][x] = BROWN
        # Spots
        g[10 + bounce][8] = LBROWN
        g[14 + bounce][15] = LBROWN
        frames.append(flatten(g))
    return frames

def make_baby():
    """Baby: small pink blob with big eyes."""
    frames = []
    for bounce in [0, 1]:
        g = make_oval(24, 24, 12, 13 - bounce, 7, 7, PINK, DRED)
        draw_eyes(g, 12, 12 - bounce, BLACK, WHITE)
        # Tiny smile
        g[15 - bounce][11] = BLACK
        g[15 - bounce][12] = BLACK
        g[15 - bounce][13] = BLACK
        # Cheek blush
        g[14 - bounce][7] = LPINK
        g[14 - bounce][16] = LPINK
        frames.append(flatten(g))
    return frames

def make_teen():
    """Teen: blue rounded creature with spiky top."""
    frames = []
    for bounce in [0, 1]:
        g = make_oval(24, 24, 12, 13 - bounce, 8, 8, LBLUE, BLUE)
        draw_eyes(g, 12, 12 - bounce, BLACK, WHITE)
        draw_mouth_smile(g, 12, 12 - bounce)
        # Spiky hair
        for x in [9, 12, 15]:
            g[4 - bounce][x] = BLUE
            g[3 - bounce][x] = BLUE
        frames.append(flatten(g))
    return frames

def make_adult():
    """Adult: larger green creature with distinct features."""
    frames = []
    for bounce in [0, 1, 0]:
        g = make_oval(24, 24, 12, 12 - bounce, 9, 9, GREEN, TEAL)
        draw_eyes(g, 12, 11 - bounce, BLACK, WHITE)
        draw_mouth_smile(g, 12, 11 - bounce)
        # Ears/horns
        g[2 - bounce][7] = TEAL
        g[2 - bounce][16] = TEAL
        g[3 - bounce][7] = GREEN
        g[3 - bounce][16] = GREEN
        frames.append(flatten(g))
    return frames

def make_elder():
    """Elder: purple creature with wrinkles."""
    frames = []
    for bounce in [0, 1]:
        g = make_oval(24, 24, 12, 12 - bounce, 9, 9, LPURPLE, PURPLE)
        draw_eyes(g, 12, 11 - bounce, BLACK, WHITE)
        # Wrinkle lines under eyes
        y = 14 - bounce
        g[y][7] = PURPLE
        g[y][8] = PURPLE
        g[y][15] = PURPLE
        g[y][16] = PURPLE
        # Small smile
        g[15 - bounce][11] = BLACK
        g[15 - bounce][12] = BLACK
        g[15 - bounce][13] = BLACK
        frames.append(flatten(g))
    return frames

def make_ghost():
    """Ghost: white translucent with wavy bottom."""
    frames = []
    for phase in [0, 1, 2]:
        g = [[T] * 24 for _ in range(24)]
        # Ghost body (top half oval, bottom wavy)
        for y in range(4, 18):
            for x in range(4, 20):
                dx = (x - 12) / 8
                dy = (y - 10) / 8
                if dx * dx + dy * dy <= 1.0:
                    g[y][x] = WHITE
        # Wavy bottom
        for x in range(4, 20):
            wave = int(math.sin((x + phase) * 1.2) * 1.5)
            base_y = 17
            for dy in range(3):
                yy = base_y + dy + wave
                if 0 <= yy < 24 and 4 <= x < 20:
                    g[yy][x] = WHITE if dy < 2 else LGRAY
        # Eyes
        for dy in range(2):
            for dx in range(2):
                g[10 + dy][8 + dx] = BLACK
                g[10 + dy][13 + dx] = BLACK
        # Open mouth
        g[14][11] = DGRAY
        g[14][12] = DGRAY
        g[15][11] = DGRAY
        g[15][12] = DGRAY
        frames.append(flatten(g))
    return frames

def make_sick():
    """Sick: greenish with X eyes and sweat drop."""
    frames = []
    for bounce in [0, 1]:
        g = make_oval(24, 24, 12, 12, 9, 9, rgb565(180, 220, 150), rgb565(100, 150, 80))
        draw_x_eyes(g, 12, 12)
        # Green-tinged mouth (wavy)
        for dx in range(-2, 3):
            y = 16 + (1 if dx % 2 == 0 else 0)
            g[y][12 + dx] = BLACK
        # Sweat drop
        if bounce == 0:
            g[5][18] = LBLUE
            g[6][18] = BLUE
        else:
            g[6][18] = LBLUE
            g[7][18] = BLUE
        frames.append(flatten(g))
    return frames

def make_happy():
    """Happy: bright yellow with big smile and sparkles."""
    frames = []
    for bounce in [0, 1]:
        g = make_oval(24, 24, 12, 12 - bounce, 9, 9, YELLOW, ORANGE)
        draw_eyes(g, 12, 11 - bounce, BLACK, WHITE)
        draw_mouth_smile(g, 12, 11 - bounce)
        # Blush
        g[14 - bounce][6] = ORANGE
        g[14 - bounce][17] = ORANGE
        # Sparkle
        if bounce == 0:
            g[3][4] = WHITE
            g[3][19] = WHITE
        else:
            g[4][3] = WHITE
            g[4][20] = WHITE
        frames.append(flatten(g))
    return frames

def make_sad():
    """Sad: dark blue with frown and tear."""
    frames = []
    for bounce in [0, 1]:
        g = make_oval(24, 24, 12, 12, 9, 9, LBLUE, BLUE)
        draw_eyes(g, 12, 11, BLACK, WHITE)
        draw_mouth_frown(g, 12, 11)
        # Tear
        ty = 14 + bounce
        g[ty][7] = CYAN
        g[ty + 1][7] = BLUE
        frames.append(flatten(g))
    return frames

def make_eating():
    """Eating: orange-tinted with open mouth, food particle."""
    frames = []
    for phase in [0, 1, 2]:
        g = make_oval(24, 24, 12, 12, 9, 9, ORANGE, BROWN)
        draw_eyes(g, 12, 11, BLACK, WHITE)
        if phase == 1:
            # Mouth closed (chewing)
            g[15][11] = BLACK
            g[15][12] = BLACK
            g[15][13] = BLACK
        else:
            draw_mouth_open(g, 12, 11)
        # Food particle
        if phase == 0:
            g[10][4] = GREEN
            g[11][4] = GREEN
            g[10][5] = GREEN
        frames.append(flatten(g))
    return frames

def make_playing():
    """Playing: bouncing with star eyes."""
    frames = []
    for phase in [0, 1, 2]:
        offset = [0, -2, -1][phase]
        g = make_oval(24, 24, 12, 12 - offset, 9, 8, CYAN, TEAL)
        # Star-shaped eyes
        draw_eyes(g, 12, 11 - offset, BLACK, YELLOW)
        # Big grin
        y = 14 - offset
        for dx in range(-3, 4):
            g[y][12 + dx] = BLACK
        g[y - 1][12 - 4] = BLACK
        g[y - 1][12 + 4] = BLACK
        # Motion lines
        if phase == 1:
            g[6][3] = LGRAY
            g[7][2] = LGRAY
            g[6][20] = LGRAY
            g[7][21] = LGRAY
        frames.append(flatten(g))
    return frames

def make_sleeping():
    """Sleeping: curled up with closed eyes and Z's."""
    frames = []
    for phase in [0, 1]:
        g = make_oval(24, 24, 12, 14, 9, 8, LPURPLE, PURPLE)
        draw_closed_eyes(g, 12, 13)
        # Slight smile
        g[16][11] = BLACK
        g[16][12] = BLACK
        g[16][13] = BLACK
        # Z's floating
        zx = 18 + phase
        zy = 5 - phase
        if 0 <= zx < 24 and 0 <= zy < 24:
            g[zy][zx] = WHITE
            if zx + 1 < 24:
                g[zy][zx + 1] = WHITE
        if 0 <= zy + 1 < 24:
            g[zy + 1][zx] = WHITE
        # Smaller Z
        sz_x = 16
        sz_y = 3
        if 0 <= sz_y < 24 and 0 <= sz_x < 24:
            g[sz_y][sz_x] = LGRAY
        frames.append(flatten(g))
    return frames


def format_array(name, idx, data):
    """Format pixel data as C array."""
    lines = [f"constexpr uint16_t sprite_{name}_frame{idx}[{len(data)}] = {{"]
    for row_start in range(0, len(data), 24):
        row = data[row_start:row_start + 24]
        lines.append("    " + ", ".join(f"0x{v:04X}" for v in row) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    sprites = [
        ("egg_idle",   make_egg(),     800, True),
        ("baby_idle",  make_baby(),    600, True),
        ("teen_idle",  make_teen(),    500, True),
        ("adult_idle", make_adult(),   400, True),
        ("elder_idle", make_elder(),   700, True),
        ("ghost",      make_ghost(),   500, True),
        ("sick",       make_sick(),    1000, True),
        ("happy",      make_happy(),   400, True),
        ("sad",        make_sad(),     800, True),
        ("eating",     make_eating(),  300, False),
        ("playing",    make_playing(), 300, False),
        ("sleeping",   make_sleeping(),1000, True),
    ]

    lines = []
    lines.append("/**")
    lines.append(" * @file SpriteData.h")
    lines.append(" * @brief Placeholder 24x24 RGB565 sprite data for TamaTac")
    lines.append(" *")
    lines.append(" * Auto-generated by generate_placeholders.py")
    lines.append(" * Replace with real pixel art via sprite2c.py")
    lines.append(" */")
    lines.append("#pragma once")
    lines.append("")
    lines.append('#include "Sprites.h"')
    lines.append("")

    # Frame data arrays
    for name, frames, delay, loop in sprites:
        for i, frame in enumerate(frames):
            lines.append(format_array(name, i, frame))
            lines.append("")

    # AnimFrame arrays
    for name, frames, delay, loop in sprites:
        frame_refs = ", ".join(f"{{sprite_{name}_frame{i}}}" for i in range(len(frames)))
        lines.append(f"constexpr AnimFrame frames_{name}[] = {{ {frame_refs} }};")
    lines.append("")

    # AnimatedSprites table
    lines.append("const AnimatedSprite animatedSprites[PET_SPRITE_COUNT] = {")
    for name, frames, delay, loop in sprites:
        loop_str = "true" if loop else "false"
        lines.append(f"    {{frames_{name}, {len(frames)}, {delay}, {loop_str}}},")
    lines.append("};")

    output = "\n".join(lines) + "\n"

    # Write to SpriteData.h
    import os
    output_path = os.path.join(os.path.dirname(__file__), "..", "..",
                               "Apps", "TamaTac", "main", "Source", "SpriteData.h")
    output_path = os.path.normpath(output_path)
    with open(output_path, "w") as f:
        f.write(output)
    print(f"Generated {output_path}")
    print(f"  {len(sprites)} sprites, {sum(len(f) for _, f, _, _ in sprites)} total frames")


if __name__ == "__main__":
    main()
