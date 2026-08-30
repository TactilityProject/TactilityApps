# Doom

A port of [doomgeneric](https://github.com/ozkl/doomgeneric) (id Software's DOOM source, GPL-2.0) for Tactility.

## Overview

Runs the classic 1993 DOOM engine directly on device, using the bundled shareware WAD or your own IWAD/PWAD from the SD card. Rendering, sound (OPL2/OPL3 FM synthesis via a DOSBox-derived emulator), and input all run natively - no external hardware or streaming required.

## Requirements

- M5Stack Tab5 (ESP32-P4) - this app is locked to that device via its manifest
- An SD card (required by the device for storage in general)
- A keyboard for gameplay: the Tab5's own keyboard accessory, or a USB keyboard. Without one attached, the game runs but can't receive input beyond touch-to-exit
- Optional: a full IWAD on the SD card (see below) for the complete game instead of the 3-level shareware episode

## Controls

- **Arrow keys / WASD**: Move and turn
- **Space / Backspace / Delete**: Use (open doors, flip switches)
- **F**: Fire
- **Enter**: Confirm (menus)
- **Tab**: Automap
- **Escape**: Menu
- **Touch the screen**: Exit back to Tactility at any time

## Adding WADs

On first run, the app creates a `doom/` folder at the root of your SD card (`sdRoot/doom/`) if it doesn't already exist, with an empty `ADD_WADS_HERE` marker file inside so it's easy to find.

Drop any of these into that folder to use them instead of the bundled shareware WAD:

- `doom2.wad`, `doom.wad`, or `doom1.wad` - checked in that order; the first one found is used as the IWAD
- `chiquito.wad` - loaded automatically as a bonus PWAD if present alongside an IWAD

If no IWAD is found on the SD card, the app falls back to the bundled shareware WAD (`doom1.wad`) automatically.

## Saves and settings

Save games and `default.cfg` (key bindings, volume, etc.) are stored in the app's own Tactility user data folder, separate from the WAD folder above - so they persist even if you're only running the bundled shareware WAD with no SD card WAD folder present.

## Known limitations

- Touch input is exit-only; there's no on-screen touch controls for movement/actions, so a keyboard is effectively required to actually play
- Locked to Tab5/ESP32-P4 for now
