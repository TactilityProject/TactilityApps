# Doom

A port of [doomgeneric](https://github.com/ozkl/doomgeneric) (id Software's DOOM source, GPL-2.0) for Tactility.

## Overview

Runs the classic 1993 DOOM engine directly on device, using the bundled shareware WAD or your own IWAD/PWAD from the SD card. Rendering, sound (OPL2/OPL3 FM synthesis via a DOSBox-derived emulator), and input all run natively - no external hardware or streaming required.

## Requirements

- An ESP32, ESP32-S3, ESP32-C6, ESP32-P4 or ESP32-S31 device with PSRAM. On the ESP32-P4 and ESP32-S31, scaling and rotation run on the PPA hardware; other chips and the linux/macOS simulator use a software scaler
- An SD card (required by the device for storage in general)
- A keyboard or a touch screen (on-screen touch controls)
- Optional: a full IWAD on the SD card (see below) for the complete game instead of the 3-level shareware episode

## Controls

- **Arrow keys / WASD**: Move and turn
- **Space / Backspace / Delete**: Use (open doors, flip switches)
- **F**: Fire
- **Enter**: Confirm (menus)
- **Tab**: Automap
- **Escape**: Menu
- **Escape, then Quit Game**: Exit back to Tactility

Touch controls are shown on devices with a touch screen. They hide when you press a key on a keyboard, and come back when you touch the screen. They sit above the status bar: a D-pad bottom-left, Fire/Use/weapon switch bottom-right, and Menu/Map in the top corners. On a portrait screen with enough room, the game moves to the top and the controls get a larger panel of their own below it. In menus Fire confirms and Use goes back. Multi-touch works, so you can move and fire at once.

## Adding WADs

On first run, the app creates a `doom/` folder at the root of your SD card (`sdRoot/doom/`) if it doesn't already exist, with an empty `ADD_WADS_HERE` marker file inside so it's easy to find.

Drop any of these into that folder to use them instead of the bundled shareware WAD:

- `doom2.wad`, `doom.wad`, or `doom1.wad` - checked in that order; the first one found is used as the IWAD
- `chiquito.wad` - loaded automatically as a bonus PWAD if present alongside an IWAD

If no IWAD is found on the SD card, the app falls back to the bundled shareware WAD (`doom1.wad`) automatically.

## Saves and settings

Save games and `default.cfg` (key bindings, volume, etc.) are stored in the app's own Tactility user data folder, separate from the WAD folder above - so they persist even if you're only running the bundled shareware WAD with no SD card WAD folder present.
