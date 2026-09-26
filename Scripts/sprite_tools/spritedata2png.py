#!/usr/bin/env python3
"""
spritedata2png.py - Extract RGB565 sprite data from SpriteData.h to PNG files

Usage:
    # Export all sprites as spritesheets (frames side-by-side):
    python spritedata2png.py ../../Apps/TamaTac/main/Source/SpriteData.h -o sprites/

    # Scale up for easier viewing/editing (4x):
    python spritedata2png.py SpriteData.h -o sprites/ --scale 4

    # Export individual frames instead of spritesheets:
    python spritedata2png.py SpriteData.h -o sprites/ --individual

Output:
    Default: One PNG per sprite as a horizontal spritesheet.
        egg_idle.png (48x24 = 2 frames), adult_idle.png (72x24 = 3 frames), etc.
    With --individual: One PNG per frame.
        egg_idle_frame0.png, egg_idle_frame1.png, etc.

    Transparent pixels (0xF81F) become fully transparent in the PNG.
    Output is compatible with sprite2c.py --spritedata for round-trip conversion.
"""

import argparse
import os
import re
import sys
from collections import OrderedDict

try:
    from PIL import Image
except ImportError:
    print("Error: Pillow is required. Install with: pip install Pillow")
    sys.exit(1)


TRANSPARENT_RGB565 = 0xF81F


def rgb565_to_rgb(val):
    """Convert RGB565 to 8-bit RGB tuple."""
    r = ((val >> 11) & 0x1F) << 3
    g = ((val >> 5) & 0x3F) << 2
    b = (val & 0x1F) << 3
    # Fill in lower bits for better color accuracy
    r |= r >> 5
    g |= g >> 6
    b |= b >> 5
    return (r, g, b)


def parse_sprite_arrays(header_text):
    """Parse all sprite_*_frame* arrays from SpriteData.h content.

    Returns:
        OrderedDict of { "sprite_name_frameN": [uint16 values, ...], ... }
    """
    pattern = re.compile(
        r'constexpr\s+uint16_t\s+(sprite_\w+)\s*\[\d+\]\s*=\s*\{([^}]+)\}\s*;',
        re.DOTALL
    )

    arrays = OrderedDict()
    hex_pattern = re.compile(r'0x([0-9A-Fa-f]+)')
    for match in pattern.finditer(header_text):
        name = match.group(1)
        data_str = match.group(2)

        values = [int(h.group(1), 16) for h in hex_pattern.finditer(data_str)]
        arrays[name] = values

    return arrays


def group_frames(arrays):
    """Group individual frame arrays by sprite name.

    Input keys like "sprite_egg_idle_frame0", "sprite_egg_idle_frame1"
    become grouped under "egg_idle" with ordered frame data.

    Returns:
        OrderedDict of { "egg_idle": [ [pixels0], [pixels1], ... ], ... }
    """
    grouped = OrderedDict()
    frame_pattern = re.compile(r'^sprite_(.+)_frame(\d+)$')

    for array_name, pixels in arrays.items():
        match = frame_pattern.match(array_name)
        if not match:
            continue
        sprite_name = match.group(1)
        frame_idx = int(match.group(2))

        if sprite_name not in grouped:
            grouped[sprite_name] = []

        # Ensure list is large enough
        while len(grouped[sprite_name]) <= frame_idx:
            grouped[sprite_name].append(None)
        grouped[sprite_name][frame_idx] = pixels

    # Remove any None gaps
    for name in grouped:
        grouped[name] = [f for f in grouped[name] if f is not None]

    return grouped


def create_frame_png(pixels, width, height, scale=1):
    """Create a PIL Image from a single frame of RGB565 pixel data."""
    img = Image.new("RGBA", (width, height))

    for y in range(height):
        for x in range(width):
            idx = y * width + x
            if idx >= len(pixels):
                break
            val = pixels[idx]

            if val == TRANSPARENT_RGB565:
                img.putpixel((x, y), (0, 0, 0, 0))
            else:
                r, g, b = rgb565_to_rgb(val)
                img.putpixel((x, y), (r, g, b, 255))

    if scale > 1:
        img = img.resize((width * scale, height * scale), Image.NEAREST)

    return img


def create_spritesheet(frame_list, width, height, scale=1):
    """Create a horizontal spritesheet from multiple frames."""
    num_frames = len(frame_list)
    sheet_width = width * num_frames
    sheet = Image.new("RGBA", (sheet_width, height))

    for i, pixels in enumerate(frame_list):
        frame_img = create_frame_png(pixels, width, height, scale=1)
        sheet.paste(frame_img, (i * width, 0))

    if scale > 1:
        sheet = sheet.resize((sheet_width * scale, height * scale), Image.NEAREST)

    return sheet


def main():
    parser = argparse.ArgumentParser(description="Extract RGB565 sprites from SpriteData.h to PNG")
    parser.add_argument("input", help="Path to SpriteData.h")
    parser.add_argument("--output", "-o", default=".", help="Output directory (default: current dir)")
    parser.add_argument("--width", "-W", type=int, default=24, help="Sprite width (default: 24)")
    parser.add_argument("--height", "-H", type=int, default=24, help="Sprite height (default: 24)")
    parser.add_argument("--scale", "-s", type=int, default=1, help="Scale factor for output PNGs (default: 1)")
    parser.add_argument("--individual", "-i", action="store_true",
                        help="Export individual frame PNGs instead of spritesheets")

    args = parser.parse_args()

    if not os.path.isfile(args.input):
        print(f"Error: {args.input} not found")
        sys.exit(1)

    os.makedirs(args.output, exist_ok=True)

    with open(args.input, "r", encoding="utf-8") as f:
        content = f.read()

    arrays = parse_sprite_arrays(content)

    if not arrays:
        print("No sprite arrays found in input file.")
        sys.exit(1)

    expected_pixels = args.width * args.height
    print(f"Found {len(arrays)} sprite frame(s) in {os.path.basename(args.input)}")
    print(f"Sprite size: {args.width}x{args.height}, output scale: {args.scale}x")
    print()

    if args.individual:
        # Export one PNG per frame
        for name, pixels in arrays.items():
            if len(pixels) != expected_pixels:
                print(f"  SKIP {name}: {len(pixels)} pixels (expected {expected_pixels})")
                continue

            img = create_frame_png(pixels, args.width, args.height, args.scale)
            output_path = os.path.join(args.output, f"{name}.png")
            img.save(output_path)
            print(f"  {name}.png")
    else:
        # Export spritesheets (one PNG per sprite, frames side-by-side)
        grouped = group_frames(arrays)

        for sprite_name, frame_list in grouped.items():
            # Validate all frames
            valid_frames = [f for f in frame_list if len(f) == expected_pixels]
            if not valid_frames:
                print(f"  SKIP {sprite_name}: no valid frames")
                continue

            if len(valid_frames) == 1:
                # Single frame: just save as-is
                img = create_frame_png(valid_frames[0], args.width, args.height, args.scale)
            else:
                # Multiple frames: create horizontal spritesheet
                img = create_spritesheet(valid_frames, args.width, args.height, args.scale)

            output_path = os.path.join(args.output, f"{sprite_name}.png")
            img.save(output_path)
            frame_desc = f"{len(valid_frames)} frame(s)"
            if len(valid_frames) > 1:
                frame_desc += f", {args.width * len(valid_frames)}x{args.height}"
            print(f"  {sprite_name}.png  ({frame_desc})")

    print(f"\nExported to {os.path.abspath(args.output)}")


if __name__ == "__main__":
    main()
