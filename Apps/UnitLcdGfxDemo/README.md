# UnitLCD Gfx Demo

A hardware test for the `SinkTarget` + `UnitLcdSink` path. It drives an M5Stack LCD Unit over
I2C while the primary display shows a status screen through the normal `gfx::Target` path.

## What it does

- Finds the first Grove I2C port.
- Tries a PaHub (channels 0-5), then a direct connection, and initialises the LCD Unit.
- Switches the unit to landscape (240×135), draws the static chrome (title, border, footer)
  directly with the unit's own primitives, and creates a sink for the animated play area only:

```cpp
UnitLcdSink sink(lcd, LCD_PLAY_LEFT, LCD_PLAY_TOP);
gfx::SinkTarget target(sink, LCD_PLAY_WIDTH, LCD_PLAY_HEIGHT, DISPLAY_COLOR_FORMAT_RGB565);
```

- Draws a bouncing ball in a 228×72 play area. The partial frame needs about 32 KB instead of the
  64.8 KB of a full 240×135 frame, which matters on boards without PSRAM. If even that does not fit
  in one contiguous block, the play area shrinks to 228×56, 228×40 or 228×24 and the border/footer
  follow it.
- Only the ball and the frame counter are redrawn after the first frame, so the I2C traffic is
  dirty regions rather than the whole frame.
- On portrait-native primary panels (e.g. Tab5), uses `gfx::Target::set_transform()` to present a
  landscape logical frame via the PPA, and scales the status text to the logical size.
- The primary display shows whether the LCD was found, its size, and whether it is behind a PaHub.
- On no-PSRAM boards (e.g. Cardputer) the LCD frame is allocated first; the primary status target is
  optional and is created only if the remaining RAM allows it. The LCD still runs without it.
- The colour of the chrome and the size of the play area are compile-time constants, so the same
  demo scales down to even smaller packs by shrinking `LCD_PLAY_HEIGHT`.

Any input exits. The LCD is cleared on startup and shutdown because its framebuffer persists across
runs, then the backlight is turned off.

## Wiring

Connect the LCD Unit to a Grove I2C port, directly or through a PaHub. The unit must be at the
standard address (`0x3E`). The app handles PaHub channel selection for each frame.

## Build

```sh
tt app build --path Apps/UnitLcdGfxDemo --architecture esp32s3
tt app bir   --path Apps/UnitLcdGfxDemo --architecture esp32s3 --host <device>
```

The app compiles `UnitLcd.cpp`, `UnitPaHub.cpp`, `Libraries/Gfx/Source`, `Libraries/Input/Source`
and the header-only `Libraries/UnitLcdGfx` adapter. It does not modify `M5UnitModules`.

## Notes

- The unit is slow compared to the primary panel; the 40 ms frame budget is a compromise between a
  visible animation and I2C bandwidth.
- `UnitLcdSink` accepts an origin, so a sink can render a sub-frame instead of the whole panel. The
  demo uses it to keep the frame buffer near 32 KB; a full frame (64.8 KB) may not fit as one
  contiguous block on no-PSRAM boards such as the Cardputer.
- The LCD is cleared on startup and shutdown because its framebuffer survives app restarts.
- If the LCD is not found, the primary screen shows an error and waits for input.
- This is also a useful reference for adding sinks for other I2C/SPI panels or OLEDs: implement
  `gfx::Sink::begin/present/end`, then hand it to `gfx::SinkTarget`.
