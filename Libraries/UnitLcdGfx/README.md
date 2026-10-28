# UnitLcdGfx

A `gfx::Sink` adapter that presents a `gfx::SinkTarget` frame to the M5Stack LCD Unit over I2C.
It deliberately lives outside `Libraries/M5UnitModules`; `UnitLcd` itself is unchanged.

## Usage

```cpp
#include <UnitLcd.h>
#include <UnitLcdSink.h>
#include <gfx/Gfx.h>

UnitLcd lcd;
if (!lcd.begin(i2c_device, UnitLcd::DEFAULT_ADDR)) { /* not found */ }
lcd.setRotation(1);                 // landscape; width()/height() define the frame size

UnitLcdSink sink(lcd);
gfx::SinkTarget target(sink, lcd.width(), lcd.height(), DISPLAY_COLOR_FORMAT_RGB565);
if (!target.valid()) { /* sink rejected the frame format/size */ }

while (running) {
    target.render([](gfx::Surface& surface) {
        surface.clear(0x0000);
        surface.drawText(4, 4, "hello", gfx::FONT_8X16, 0xFFFF);
    });
}
```

The `SinkTarget` owns a full RGB565 frame buffer and presents only dirty regions; the sink sets
the LCD window and streams each changed row with `UnitLcd::writePixels()`.

## Build integration

An app using this library must compile `UnitLcdGfx/Source` (currently header-only, so nothing
extra), `M5UnitModules/Source/UnitLcd.cpp`, `Libraries/Gfx/Source/*.c*`, and include:

- `Libraries/Gfx/Include`
- `Libraries/M5UnitModules/Include`
- `Libraries/UnitLcdGfx/Include`

The usual `REQUIRES graphics-module` and the platform I2C driver requirements from
`M5UnitModules` also apply. `UnitLcd` uses a 400 kHz I2C bus and a 135x240 (or rotated) panel, so
prefer dirty regions and avoid full-screen animation.

## Notes

- Only `DISPLAY_COLOR_FORMAT_RGB565` is supported, matching the unit's protocol.
- The unit's rotation must be set before creating the `SinkTarget`; `begin()` checks that the
  requested frame size matches `lcd.width()`/`lcd.height()`.
- `UnitLcdSink` holds a reference to the `UnitLcd`; the LCD must outlive the sink and target.
- The unit's framebuffer persists across app restarts. If an app doesn't repaint every pixel,
  call `lcd.fillScreen(0x0000)` at startup and shutdown (the demo does this) so the previous
  image doesn't linger.
