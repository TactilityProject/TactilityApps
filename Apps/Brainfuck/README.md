# Brainfuck

A Brainfuck esoteric programming language interpreter for Tactility.

## Overview

Run Brainfuck programs on your device. Includes built-in examples and support for loading custom scripts. Features cycle limit protection to prevent infinite loops from locking up the device.

## Features

- Full Brainfuck VM with 4096-byte tape
- Cycle limit protection (2,000,000 cycles max)
- Built-in examples (Hello World, Fibonacci, Alphabet, Beer song)
- Multi-line code editor
- Execution statistics (cycle count display)
- Load custom scripts from user data directory

## Controls

- **Run Button**: Execute Brainfuck code
- **Enter Key**: Run code from input area
- **Toolbar Buttons**:
  - Trash icon: Clear output and input
  - List icon: Toggle between examples/scripts list and editor

## Custom Scripts

Place `.bf` or `.b` files (up to 32 KB each) in the app's user data directory to load them from the app. The directory path is shown in the script list when no scripts are found.

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
