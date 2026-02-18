# Brainfuck

A Brainfuck esoteric programming language interpreter for Tactility.

## Overview

Run Brainfuck programs on your device. Includes built-in examples and support for loading scripts from the SD card. Features cycle limit protection to prevent infinite loops from locking up the device.

## Features

- Full Brainfuck VM with 4096-byte tape
- Cycle limit protection (2,000,000 cycles max)
- Built-in examples (Hello World, Fibonacci, Alphabet, Beer song)
- Multi-line code editor
- Execution statistics (cycle count display)
- Load scripts from SD card

## Controls

- **Run Button**: Execute Brainfuck code
- **Enter Key**: Run code from input area
- **Toolbar Buttons**:
  - Trash icon: Clear output and input
  - List icon: Toggle between examples/scripts list and editor

## SD Card Support

Place `.bf` or `.b` files (up to 32 KB each) in `/sdcard/tactility/brainfuck/` to load them from the app.

## Brainfuck Reference

| Command | Description |
|---------|-------------|
| `>` | Move pointer right |
| `<` | Move pointer left |
| `+` | Increment byte at pointer |
| `-` | Decrement byte at pointer |
| `.` | Output byte as ASCII |
| `,` | Input byte (not supported) |
| `[` | Jump forward past `]` if byte is zero |
| `]` | Jump back to `[` if byte is nonzero |

## Requirements

- Tactility
- Touchscreen or keyboard
