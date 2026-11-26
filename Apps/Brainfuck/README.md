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
- Open and save scripts

## Controls

Type code in the top field, then press **Play**. The output appears below, with the result in the status line.

- **Play**: execute the code
- **Menu**: New, Open, Save, Save as, and the built-in examples

## Files

Scripts are `.bf` or `.b` files of up to 32 KB. Open and Save as start in the app's user data directory.

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
