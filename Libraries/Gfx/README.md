# Gfx

Direct-to-display rendering for Tactility apps, outside LVGL.

`gfx::Target` takes a display device and picks the cheapest way to get pixels onto the panel:

| Panel | Mode | How |
|---|---|---|
| Two hardware frame buffers (MIPI DSI, RGB) | `Flip` | draw into the back buffer, present by flipping |
| One full-frame buffer fits | `FullFrame` | draw once, send only the dirty regions |
| Little RAM (ESP32, no PSRAM) | `Banded` | render and present one band at a time |
| Mono / `REQUIRES_FULL_FRAME` (e-paper) | `FullFrame` | always send whole frames |

Apps draw into a `gfx::Surface` and never need to know which mode was picked:

```cpp
#include <gfx/Gfx.h>

gfx::Target target(display_device);
if (!target.valid()) { /* unsupported display */ }

while (running) {
    advanceState();
    target.render([this](gfx::Surface& surface) {
        surface.fillRect(x, y, w, h, 0x0000);   // clear what moves
        surface.line(x0, y0, x1, y1, 0xF800);   // draw
    });
}
```

See [DESIGN.md](DESIGN.md) for the architecture and the roadmap (images, canvases, extra fonts,
custom presentation backends).

## Coordinates and clipping

Surface coordinates are world coordinates in the display's pixel space: `x` in `[0, width())`,
`y` in `[originY(), originY() + height())`. In banded mode the callback runs once per band with a
surface covering only that band, so drawing outside it is clipped for free. The callback must not
mutate app state: it can run more than once per frame.

A surface also has a clip rectangle, defaulting to its full bounds. Restrict drawing to a region
without changing the render mode:

```cpp
surface.setClip({ 10, 10, 100, 60 });
surface.fillRect(0, 0, 200, 200, 0x001F);   // only the part inside the clip is written
surface.resetClip();
```

## Primitives

```cpp
surface.clear(uint16_t rgb565 = 0x0000);
surface.pixel(x, y, rgb565);

surface.hline(x, y, length, rgb565);
surface.vline(x, y, length, rgb565);
surface.line(x0, y0, x1, y1, rgb565);
surface.fillRect(x, y, width, height, rgb565);
surface.drawRect(x, y, width, height, rgb565);

surface.fillCircle(cx, cy, radius, rgb565);
surface.drawCircle(cx, cy, radius, rgb565);
surface.fillEllipse(cx, cy, radiusX, radiusY, rgb565);
surface.drawEllipse(cx, cy, radiusX, radiusY, rgb565);
surface.fillRoundRect(x, y, width, height, radius, rgb565);
surface.drawRoundRect(x, y, width, height, radius, rgb565);
surface.fillTriangle(x0, y0, x1, y1, x2, y2, rgb565);
surface.drawTriangle(x0, y0, x1, y1, x2, y2, rgb565);

// Angles: 0 = right, 90 = down, 180 = left, 270 = up.
surface.fillArc(cx, cy, outerRadius, innerRadius, startDegrees, endDegrees, rgb565);
surface.drawArc(cx, cy, outerRadius, innerRadius, startDegrees, endDegrees, rgb565);

surface.gradient({ x, y, x + w, y + h }, fromRgb565, toRgb565);
```

All colours are RGB565. `Surface` converts to the panel's native format (including RGB888,
GRAYSCALE8 and MONOCHROME) as it writes.

## Text and fonts

Text is drawn from bitmap fonts. Built-in fonts:

| Font | Size | Notes |
|---|---|---|
| `gfx::FONT_5X7` | 5x7, 6px advance | Classic terminal font |
| `gfx::FONT_5X7_PROPORTIONAL` | 5x7, per-glyph advance | Same glyphs, proportional spacing |
| `gfx::FONT_8X8` | 8x8, 8px advance | Public-domain VGA-style font |
| `gfx::FONT_8X16` | 8x16, 8px advance | Terminus body font, Latin-1 (OFL-1.1; see `Licenses/Terminus-OFL.txt`) |

`FONT_8X16` at `scale = 2` gives a 16x32 cell for large display text.

```cpp
// Transparent: only glyph pixels are written.
surface.drawText(8, 8, "Hello", gfx::FONT_8X8, 0xFFFF);

// Opaque: each glyph cell gets a background, including the gap after the glyph.
surface.drawText(8, 32, "Hello", gfx::FONT_5X7, 0xFFFF, 0x0000, 2);
```

For layout, use `TextStyle`:

```cpp
gfx::TextStyle style;
style.font = &gfx::FONT_5X7;
style.foreground = 0xFFFF;
style.background = 0x0010;
style.opaque = true;
style.scale = 2;
style.align = gfx::TextAlign::Center;
style.verticalAlign = gfx::TextVerticalAlign::Middle;
style.wrap = gfx::TextWrap::Word;

// x/y are the anchor for Center/Right and Middle/Bottom.
surface.drawText(160, 120, "Centered", style);

// Or lay out inside a box, clipped to it.
surface.drawText({ 8, 8, 312, 60 }, "A longer line that wraps at spaces", style);
```

Measurement:

```cpp
int width = gfx::measureText("Hello", gfx::FONT_8X8);
int height = gfx::measureTextHeight("Hello", gfx::FONT_8X8);

// Tabs use style.tabSize cells; measureText() defaults to 4.
int tabbed = gfx::measureText("a\tb", gfx::FONT_5X7, 1, 2);
```

`TextStyle` also supports proportional fonts, configurable tab width, and ellipsis:

```cpp
gfx::TextStyle style;
style.font = &gfx::FONT_5X7_PROPORTIONAL;
style.foreground = 0xFFFF;
style.tabSize = 2;
style.ellipsis = true;

// A line too wide for the box is truncated with "...".
surface.drawText({ 8, 8, 120, 24 }, "A very long line", style);
```

`FONT_5X7_PROPORTIONAL` uses per-glyph widths and advances while sharing the 5x7 glyph
data. `Font` can carry optional `glyphWidths` and `advances` tables for app-supplied
proportional fonts.

`TextStyle::scale` is an integer magnification. Small text should use a native font; large display
text can be scaled. A `Font` is plain data (`glyphWidth`, `glyphHeight`, `advance`, `lineHeight`,
layout and a pointer to 1bpp glyphs), so apps can supply their own font without changing the
library.

The text pipeline is byte-oriented: 5x7 and 8x8 cover ASCII, and `FONT_8X16` also has Latin-1
glyphs. UTF-8 decoding is not implemented yet, so multi-byte UTF-8 sequences render as individual
bytes (or `?` when the selected font has no glyph for that byte).

## Images and masks

`Surface::blit()` copies a region from another `PixelBuffer`, clipping against both the source
buffer and the destination clip, and converting formats as needed:

```cpp
PixelBuffer* source = pixel_buffer_create(DISPLAY_COLOR_FORMAT_RGB565, 32, 32);
gfx::Surface sprite(source);
sprite.fillCircle(16, 16, 12, 0xF800);

surface.blit(*source, 0, 0, 32, 32, x, y);

// Treat black as transparent (colour key).
surface.blitTransparent(*source, 0, 0, 32, 32, x, y, 0x0000);
```

Same-format blits use per-row `memcpy`; transparent blits over a fast RGB565 format compare native
pixels directly. Cross-format blits go through RGB565 conversion per pixel.

1bpp and 8bpp masks cover icons and soft edges:

```cpp
// 1bpp mask, MSB first, one byte per row.
surface.drawMask(mask, maskStride, width, height, x, y, 0xFFFF, true);

// 8bpp alpha mask: 0 = transparent, 255 = solid.
surface.drawAlphaMask(alpha, alphaStride, width, height, x, y, 0xFFFF);

// Blend a single pixel.
surface.blendPixel(x, y, 0xF800, 128);
```

The Material Symbols data already used by some apps is 2bpp LVGL font data rather than a 1bpp
mask, so a 2bpp glyph decoder is a future addition; the 1bpp/8bpp paths are independent of it.

## Polygons

```cpp
const gfx::Point star[] = {
    { 50, 10 }, { 61, 40 }, { 95, 40 }, { 68, 58 }, { 80, 90 },
    { 50, 70 }, { 20, 90 }, { 32, 58 }, { 5, 40 }, { 39, 40 },
};

surface.fillPolygon(star, 10, 0xF800);   // even-odd scanline fill
surface.drawPolygon(star, 10, 0xFFFF);   // outline
```

`fillPolygon()` supports up to `gfx::Surface::MAX_POLYGON_POINTS` (32) vertices and marks its
bounding box dirty once, like the other compound primitives.

## Off-screen surfaces

A `Surface` does not need a `Target`. Wrap any `PixelBuffer` directly to compose sprites, render
into an app-owned canvas, or drive a panel that is not a Tactility display device:

```cpp
#include <gfx/Gfx.h>
#include <graphics/pixel_buffer.h>

PixelBuffer* buffer = pixel_buffer_create(DISPLAY_COLOR_FORMAT_RGB565, 128, 64);
gfx::Surface surface(buffer);
surface.clear(0x0841);
surface.drawText(4, 4, "canvas", gfx::FONT_5X7, 0xFFFF);
// ... hand pixel_buffer_get_data(buffer) to whatever presents it ...
pixel_buffer_free(buffer);
```

Sub-views make it easy to draw into part of a larger buffer:

```cpp
gfx::Surface view(buffer, { 16, 16, 48, 32 });  // x0, y0, x1, y1
view.fillCircle(16, 8, 8, 0xF800);              // coordinates start at the view's top-left
```

`gfx::Canvas` owns the buffer for you and keeps the `Surface` in sync:

```cpp
gfx::Canvas sprite(32, 32, DISPLAY_COLOR_FORMAT_RGB565);
if (sprite.valid()) {
    sprite.clear(0xF81F);                        // magenta colour key
    sprite.surface().fillCircle(16, 16, 12, 0x07FF);
    sprite.surface().drawText(4, 12, "OK", gfx::FONT_5X7, 0xFFFF);
}

target.render([&](gfx::Surface& s) {
    s.blitTransparent(*sprite.buffer(), 0, 0, 32, 32, x, y, 0xF81F);
});
```

## Presentation sinks

`gfx::Target` is the optimized display path. For an I2C LCD, a headless bitmap, or any other
backend, implement `gfx::Sink` and let `gfx::SinkTarget` own the frame buffer and dirty tracking:

```cpp
class MySink : public gfx::Sink {
public:
    bool begin(const gfx::SinkInfo& info) override { /* open the backend */ return true; }
    bool present(const PixelBuffer& buffer, const gfx::Rect& rect) override {
        // send buffer's rows [rect.y0, rect.y1), columns [rect.x0, rect.x1)
        return true;
    }
    void end() override { /* close the backend */ }
};

MySink sink;
gfx::SinkTarget target(sink, 128, 64, DISPLAY_COLOR_FORMAT_RGB565);
if (target.valid()) {
    target.render([](gfx::Surface& surface) {
        surface.clear(0x0000);
        surface.drawText(4, 4, "sink", gfx::FONT_8X16, 0xFFFF);
    });
}
```

- `SinkTarget` owns one full-frame `PixelBuffer`, records dirty regions, and calls
  `Sink::present()` for each changed rect. The buffer persists between frames.
- Override `frame_buffer_policy()` when the backend needs DMA-capable memory; otherwise the frame
  buffer prefers external RAM.
- `gfx::DisplaySink` adapts a Tactility display device using `display_draw_bitmap()` (whole rows).
  `gfx::Target` remains the better choice for displays because it adds flip, banded rendering and
  hardware transform support.
- When memory is tight, `Target::release()` frees the primary render buffers; the panel keeps its
  last presented frame. The UnitLCD demo instead allocates the LCD frame first and creates the
  optional primary status target afterwards.
- `UnitLcdSink` lives in `Libraries/UnitLcdGfx` and presents to the M5Stack LCD Unit without
  changing `M5UnitModules`. It takes an origin, so it can own a sub-frame: draw the static chrome
  with the unit's own primitives and let the sink drive only the part that changes. A smaller frame
  buffer is often the difference between working and not working on no-PSRAM boards.

## What the callback must guarantee

Buffers start black. In banded mode the target clears each band before calling back. In flip and
full-frame mode the buffer persists between frames, so **clear a region before drawing into it
again** — the target only knows about the pixels you report.

Primitives mark their own dirty regions. If you write through a raw pointer, call `markDirty()`.

## Memory

- Banded mode uses a 32 KB budget by default and keeps bands in internal DMA-capable memory,
  because SPI panel drivers DMA straight from the buffer and ESP32-class chips can't DMA from PSRAM.
- Full-frame mode requires the frame to fit with 2x headroom, and requires DMA-capable memory
  unless the display reports `PREFER_EXTERNAL_RAM` (meaning its driver copies the data).

## Presenting

`DisplayApi::draw_bitmap()` has no stride parameter: the driver assumes the pixels for the rectangle
it is given are tightly packed. Dirty rectangles are therefore copied through a small staging buffer
(a few tens of KB, internal DMA memory) before being presented, so a driver only has to drive the
rectangle itself — which matters a lot on e-paper, where the panel is refreshed per region. When no
staging buffer could be allocated, presenting falls back to whole rows.

Banded mode sends whole bands, which are only a few rows tall.

No widgets, no event loop. This is surfaces, primitives, text and presenting. `pixel_buffer_*` from the
`graphics-module` provides the buffers, format conversion and cache writeback; `gfx` adds the
drawing and the present policy on top.

## Example app

[`Apps/GfxBenchmark`](../../Apps/GfxBenchmark) is a PDQ-style primitive benchmark built on this
library. It runs the same phases as the M5UnitTest LCD GFX test through `gfx::Target`, adapts the
workload to the display, and draws its results table with the new font/text API. It is a good
end-to-end workout for new primitives and render modes.

## Licenses

The library code follows [LICENSE.md](LICENSE.md). The 8x16 Terminus font data in
`Source/FontTerminus16.cpp` is licensed under the SIL Open Font License 1.1; see
[Licenses/Terminus-OFL.txt](Licenses/Terminus-OFL.txt). The 5x7 and 8x8 fonts are public-domain
bitmap fonts.
