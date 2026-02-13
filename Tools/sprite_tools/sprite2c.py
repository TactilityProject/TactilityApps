#!/usr/bin/env python3
"""
sprite2c.py - Convert PNG sprites to RGB565 C arrays for TamaTac

Usage:
    # Single sprite (24x24 PNG):
    python sprite2c.py egg_idle.png --name egg_idle

    # Spritesheet (multiple 24x24 frames in a horizontal strip):
    python sprite2c.py egg_spritesheet.png --name egg_idle --cols 2

    # Custom sprite size:
    python sprite2c.py big_sprite.png --name adult_idle --width 32 --height 32

    # Custom transparent color (default: uses alpha channel):
    python sprite2c.py sprite.png --name test --transparent 255,0,255

    # Batch convert all PNGs in a directory:
    python sprite2c.py sprites_dir/ --batch

    # Generate complete SpriteData.h from a directory of PNGs:
    python sprite2c.py sprites_dir/ --spritedata -o SpriteData.h
    (See --spritedata section below for naming conventions)

Output:
    Generates a .h file with constexpr uint16_t arrays ready for inclusion.
    Transparent pixels become 0xF81F (magenta in RGB565).

SpriteData mode (--spritedata):
    Expects PNGs named: <sprite_name>.png (single frame) or <sprite_name>.png
    with multiple columns for spritesheets.

    Sprite names must match SpriteId enum order in Sprites.h:
        egg_idle, baby_idle, teen_idle, adult_idle, elder_idle,
        ghost, sick, happy, sad, eating, playing, sleeping

    Each PNG can be a single frame or a horizontal spritesheet.
    The script auto-detects frame count from image width.

    Animation config (frameDelayMs, loop) is specified via a config file
    or defaults. Place a sprite_config.txt in the sprite directory:
        egg_idle,800,true
        baby_idle,600,true
        adult_idle,400,true
        eating,300,false
        playing,300,false
        ...
"""

import argparse
import os
import sys

try:
    from PIL import Image
except ImportError:
    print("Error: Pillow is required. Install with: pip install Pillow")
    sys.exit(1)


TRANSPARENT_RGB565 = 0xF81F  # Magenta in RGB565

# SpriteId enum order (must match Sprites.h)
SPRITE_NAMES = [
    "egg_idle", "baby_idle", "teen_idle", "adult_idle", "elder_idle",
    "ghost", "sick", "happy", "sad", "eating", "playing", "sleeping",
]

# Default animation config: (frameDelayMs, loop)
DEFAULT_ANIM_CONFIG = {
    "egg_idle":    (800, True),
    "baby_idle":   (600, True),
    "teen_idle":   (500, True),
    "adult_idle":  (400, True),
    "elder_idle":  (700, True),
    "ghost":       (500, True),
    "sick":        (1000, True),
    "happy":       (400, True),
    "sad":         (800, True),
    "eating":      (300, False),
    "playing":     (300, False),
    "sleeping":    (1000, True),
}


def rgb_to_rgb565(r, g, b):
    """Convert 8-bit RGB to RGB565."""
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def convert_sprite(image, frame_width, frame_height, transparent_color=None, cols=None):
    """Convert a PIL Image to a list of RGB565 frame arrays.

    Args:
        image: PIL Image (RGBA or RGB)
        frame_width: Width of each frame
        frame_height: Height of each frame
        transparent_color: Optional (R,G,B) tuple for color-key transparency
        cols: Number of columns to extract (auto-detected if None)

    Returns:
        List of lists of uint16_t values (one list per frame)
    """
    img = image.convert("RGBA")
    img_width, img_height = img.size

    max_cols = img_width // frame_width
    cols = cols if cols is not None else max_cols
    rows = img_height // frame_height

    if cols == 0 or rows == 0:
        print(f"Error: Image {img_width}x{img_height} is smaller than frame size {frame_width}x{frame_height}")
        sys.exit(1)

    frames = []
    for row in range(rows):
        for col in range(cols):
            frame = []
            for y in range(frame_height):
                for x in range(frame_width):
                    px = img.getpixel((col * frame_width + x, row * frame_height + y))
                    r, g, b, a = px

                    if transparent_color and (r, g, b) == transparent_color:
                        frame.append(TRANSPARENT_RGB565)
                    elif a < 128:
                        frame.append(TRANSPARENT_RGB565)
                    else:
                        val = rgb_to_rgb565(r, g, b)
                        # Avoid accidental transparency
                        if val == TRANSPARENT_RGB565:
                            val = 0xF81E  # Slightly different magenta
                        frame.append(val)
            frames.append(frame)

    return frames


def format_array(name, frame_idx, data, width):
    """Format a single frame as a C constexpr array."""
    lines = []
    lines.append(f"constexpr uint16_t sprite_{name}_frame{frame_idx}[{len(data)}] = {{")

    for row_start in range(0, len(data), width):
        row = data[row_start:row_start + width]
        hex_vals = ", ".join(f"0x{v:04X}" for v in row)
        lines.append(f"    {hex_vals},")

    lines.append("};")
    return "\n".join(lines)


def generate_header(name, frames, width, height):
    """Generate complete .h file content for a sprite."""
    lines = []
    lines.append(f"// Auto-generated by sprite2c.py - {name}")
    lines.append(f"// {len(frames)} frame(s), {width}x{height} RGB565")
    lines.append(f"// Transparent color key: 0x{TRANSPARENT_RGB565:04X} (magenta)")
    lines.append("")
    lines.append("#pragma once")
    lines.append("#include <cstdint>")
    lines.append("")

    for i, frame in enumerate(frames):
        lines.append(format_array(name, i, frame, width))
        lines.append("")

    return "\n".join(lines)


def load_anim_config(sprite_dir):
    """Load animation config from sprite_config.txt if it exists.

    Format: name,delayMs,loop (one per line)
    Returns dict of { name: (delayMs, loop) }
    """
    config = dict(DEFAULT_ANIM_CONFIG)
    config_path = os.path.join(sprite_dir, "sprite_config.txt")
    if os.path.isfile(config_path):
        with open(config_path, "r") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split(",")
                if len(parts) == 3:
                    try:
                        name = parts[0].strip()
                        delay = int(parts[1].strip())
                        loop = parts[2].strip().lower() == "true"
                        config[name] = (delay, loop)
                    except ValueError as e:
                        print(f"  WARNING: Invalid config line '{line}': {e}")
                else:
                    print(f"  WARNING: Malformed config line '{line}'")
    return config


def generate_spritedata(sprite_dir, frame_width, frame_height, transparent_color):
    """Generate complete SpriteData.h from a directory of sprite PNGs.

    Expects PNGs named to match SPRITE_NAMES entries.
    """
    anim_config = load_anim_config(sprite_dir)

    lines = []
    lines.append("/**")
    lines.append(" * @file SpriteData.h")
    lines.append(f" * @brief {frame_width}x{frame_height} RGB565 sprite data for TamaTac")
    lines.append(" *")
    lines.append(" * Auto-generated by sprite2c.py --spritedata")
    lines.append(" * Re-generate with: python sprite2c.py <sprites_dir> --spritedata -o SpriteData.h")
    lines.append(" */")
    lines.append("#pragma once")
    lines.append("")
    lines.append('#include "Sprites.h"')
    lines.append("")

    # Track frames per sprite for the animation table
    sprite_frame_counts = {}
    total_frames = 0

    # Process each sprite
    for sprite_name in SPRITE_NAMES:
        png_path = os.path.join(sprite_dir, f"{sprite_name}.png")
        if not os.path.isfile(png_path):
            print(f"  WARNING: {sprite_name}.png not found, skipping")
            sprite_frame_counts[sprite_name] = 0
            continue

        with Image.open(png_path) as img:
            frames = convert_sprite(img, frame_width, frame_height, transparent_color)
        sprite_frame_counts[sprite_name] = len(frames)
        total_frames += len(frames)

        print(f"  {sprite_name}: {len(frames)} frame(s) from {sprite_name}.png")

        # Write pixel arrays
        for i, frame in enumerate(frames):
            lines.append(format_array(sprite_name, i, frame, frame_width))
            lines.append("")

    # Write AnimFrame arrays
    for sprite_name in SPRITE_NAMES:
        count = sprite_frame_counts.get(sprite_name, 0)
        if count == 0:
            continue
        frame_refs = ", ".join(
            f"{{sprite_{sprite_name}_frame{i}}}" for i in range(count)
        )
        lines.append(f"constexpr AnimFrame frames_{sprite_name}[] = {{ {frame_refs} }};")

    lines.append("")

    # Write animatedSprites table
    lines.append("const AnimatedSprite animatedSprites[PET_SPRITE_COUNT] = {")
    for sprite_name in SPRITE_NAMES:
        count = sprite_frame_counts.get(sprite_name, 0)
        if count == 0:
            # Fallback: empty entry (shouldn't happen with complete assets)
            lines.append(f"    {{nullptr, 0, 0, false}},  // {sprite_name} MISSING")
            continue
        delay, loop = anim_config.get(sprite_name, (500, True))
        loop_str = "true" if loop else "false"
        lines.append(f"    {{frames_{sprite_name}, {count}, {delay}, {loop_str}}},")

    lines.append("};")
    lines.append("")

    processed_count = sum(1 for c in sprite_frame_counts.values() if c > 0)
    print(f"\nTotal: {processed_count} sprites processed, {total_frames} frames")
    return "\n".join(lines)


def process_file(filepath, name, frame_width, frame_height, transparent_color, cols=None):
    """Process a single PNG file and return header content."""
    with Image.open(filepath) as img:
        frames = convert_sprite(img, frame_width, frame_height, transparent_color, cols)
    print(f"  {name}: {len(frames)} frame(s) from {os.path.basename(filepath)}")
    return generate_header(name, frames, frame_width, frame_height)


def main():
    parser = argparse.ArgumentParser(description="Convert PNG sprites to RGB565 C arrays")
    parser.add_argument("input", help="Input PNG file or directory (with --batch or --spritedata)")
    parser.add_argument("--name", "-n", help="Sprite name for output (default: filename without extension)")
    parser.add_argument("--width", "-W", type=int, default=24, help="Frame width in pixels (default: 24)")
    parser.add_argument("--height", "-H", type=int, default=24, help="Frame height in pixels (default: 24)")
    parser.add_argument("--cols", "-c", type=int, help="Number of columns in spritesheet (auto-detected if omitted)")
    parser.add_argument("--transparent", "-t", help="Transparent color as R,G,B (default: use alpha channel)")
    parser.add_argument("--output", "-o", help="Output file path (default: <name>.h)")
    parser.add_argument("--batch", "-b", action="store_true", help="Process all PNGs in directory")
    parser.add_argument("--spritedata", "-S", action="store_true",
                        help="Generate complete SpriteData.h from directory of sprite PNGs")

    args = parser.parse_args()

    transparent_color = None
    if args.transparent:
        parts = args.transparent.split(",")
        if len(parts) != 3:
            print("Error: --transparent must be R,G,B (e.g., 255,0,255)")
            sys.exit(1)
        try:
            transparent_color = tuple(int(p.strip()) for p in parts)
            if not all(0 <= c <= 255 for c in transparent_color):
                raise ValueError("RGB values must be 0-255")
        except ValueError as e:
            print(f"Error: --transparent must be R,G,B with values 0-255 (e.g., 255,0,255): {e}")
            sys.exit(1)

    if args.spritedata:
        if not os.path.isdir(args.input):
            print(f"Error: {args.input} is not a directory")
            sys.exit(1)

        print(f"Generating SpriteData.h from {args.input}")
        print(f"Frame size: {args.width}x{args.height}")
        print()

        content = generate_spritedata(args.input, args.width, args.height, transparent_color)

        output_path = args.output or "SpriteData.h"
        with open(output_path, "w") as f:
            f.write(content)
        print(f"\nOutput: {output_path}")

    elif args.batch:
        if not os.path.isdir(args.input):
            print(f"Error: {args.input} is not a directory")
            sys.exit(1)

        png_files = sorted(f for f in os.listdir(args.input) if f.lower().endswith(".png"))
        if not png_files:
            print(f"No PNG files found in {args.input}")
            sys.exit(1)

        print(f"Processing {len(png_files)} files...")
        for png_file in png_files:
            filepath = os.path.join(args.input, png_file)
            name = os.path.splitext(png_file)[0]
            content = process_file(filepath, name, args.width, args.height, transparent_color, args.cols)

            output_path = os.path.join(args.input, f"{name}.h")
            with open(output_path, "w") as f:
                f.write(content)
            print(f"  -> {output_path}")
    else:
        if not os.path.isfile(args.input):
            print(f"Error: {args.input} not found")
            sys.exit(1)

        name = args.name or os.path.splitext(os.path.basename(args.input))[0]
        content = process_file(args.input, name, args.width, args.height, transparent_color, args.cols)

        output_path = args.output or f"{name}.h"
        with open(output_path, "w") as f:
            f.write(content)
        print(f"Output: {output_path}")


if __name__ == "__main__":
    main()
