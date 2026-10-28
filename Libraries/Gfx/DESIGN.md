# Gfx design

This is the working design for turning `Libraries/Gfx` from a direct-to-display presenter into a
general 2D drawing library. It is deliberately written down so the API can be reviewed before the
larger pieces (images, canvases, extra fonts) are built.

Status: steps 1-4 are implemented (public `Surface` with clipping, primitives, fonts/text,
blitting, transparency, masks, `Canvas`, polygons, alpha blending, an 8x16 body font,
proportional/layout improvements, and the generic Sink/SinkTarget presentation path). Next: step 5
(pixel_buffer helpers, after a discussion with ByteWelder).

## Goals

- One drawing API that works on every panel `gfx::Target` supports: MIPI DSI, RGB, SPI, e-paper,
  monochrome, grayscale, and panels with or without PSRAM.
- Enough primitives and text to build a small UI without LVGL.
- Usable off-screen: the same `Surface` code draws into an owned buffer, a sprite, or a frame
  buffer, not only inside `Target::render()`.
- No heap allocation in the drawing path. Fonts and geometry live in flash/stack.
- Keep `Target` responsible for presentation policy and `Surface` responsible for pixels.

## Non-goals

- Widgets, layout engines, an event loop, animation timelines.
- A full vector/raster engine (anti-aliasing, Bézier curves, transforms) in the current steps.
- Replacing LVGL. This is for apps that deliberately render outside LVGL.

## Layers

```
        app drawing code
               |
            gfx::Surface        <- primitives, text, clip, dirty tracking
               |
        PixelBuffer (graphics-module)
               |                  <- storage, format conversion, cache sync
        gfx::Target               <- flip / full-frame / banded / PPA transform
               |
        display driver
```

- `PixelBuffer` is the storage primitive. It knows the format, stride and cache maintenance.
  `gfx::Surface` is a view over one. A `Surface` never owns the buffer.
- `Target` decides how a frame reaches the panel. It creates one `Surface` per frame (or one per
  band) and hands it to the render callback.
- Drawing code only sees `Surface`, so the same code works in flip, full-frame and banded modes.

## Coordinate model

- All coordinates are **world coordinates** in the display's pixel space.
- A `Surface` has `bounds()`: the world rectangle it covers. In banded mode that is
  `{0, bandY, width, bandY + bandRows}`.
- A `Surface` also has a **clip rectangle**, defaulting to `bounds()`. Every primitive is clipped to
  it. `setClip()` lets an app restrict drawing to a panel sub-region or a scroll viewport.
- A public `Surface(PixelBuffer*)` covers the whole buffer with its origin at `(0, 0)`. A
  `Surface(PixelBuffer*, Rect view)` is a sub-view of a larger buffer; its drawing coordinates
  start at the view's top-left.
- All drawing is clipped, never wrapped or reflected.

## Dirty tracking

`Target` presents per dirty region. `Surface` owns the `DirtyRegions` set for the current frame:

- Simple primitives (`fillRect`, `line`, `pixel`, ...) clip first, then mark the visible rectangle.
- Compound primitives (circle, triangle, text, ...) mark their bounding box **once**, then draw
  their spans without touching the dirty set. This matters because `DirtyRegions` has a small
  fixed capacity; one entry per glyph or per scanline would collapse into a whole-frame present.
- `DirtyRegions::add()` merges the new rectangle into every existing rectangle it intersects, so a
  clear plus a redraw usually collapses back to one entry.

Apps that write through a raw pointer must call `markDirty()` themselves.

`Surface::pixel()` marks its pixel. Internal span/line helpers do not; that is what keeps compound
primitives cheap.

## Fonts and text

Text is data-driven, not hard-coded to one font.

```cpp
struct Font {
    uint8_t glyphWidth;    // glyph cell width in pixels
    uint8_t glyphHeight;   // glyph cell height in pixels
    uint8_t advance;       // horizontal step at scale 1 (glyphWidth + gap)
    uint8_t lineHeight;    // vertical step at scale 1
    uint8_t firstChar;
    uint8_t lastChar;
    FontLayout layout;     // Rows (bit 7 = left), RowsLsb (bit 0 = left) or Columns (bit 0 = top)
    const uint8_t* data;   // packed 1bpp glyphs
};
```

- Fonts are selected at runtime by passing a `const Font&`. Apps can supply their own font data
  without changing the library. `FONT_5X7` uses `Columns`; the public-domain `FONT_8X8` uses
  `RowsLsb` because its source stores bit 0 as the leftmost pixel. `FONT_8X16` is Terminus
  (OFL-1.1), and `FONT_5X7_PROPORTIONAL` shares the 5x7 glyph data with per-glyph widths/advances.
- Sizes come from two mechanisms: native fonts (5x7, 8x8, 8x16) and integer scaling
  (`TextStyle::scale`). Small text should use a native font; large display text can be scaled
  (8x16 at scale 2 is 16x32).
- `TextStyle` carries font, foreground, optional background, scale, tab width, ellipsis,
  horizontal/vertical alignment and wrapping. `Font` can carry optional per-glyph width/advance
  tables for proportional fonts.
- Text is drawn as horizontal spans (runs of set bits), not one `writePixel()` per pixel. That makes
  it fast on RGB565 and still correct on grayscale/monochrome, because spans go through the same
  format conversion as `fillRect()`.
- Dirty tracking marks the whole text block once.
- The text pipeline treats bytes as characters. The 5x7/8x8 fonts are ASCII; the 8x16 Terminus
  font also has Latin-1 glyphs. A UTF-8 decoder can be added later without changing the `Font`
  structure (glyphs would be indexed by code point; the current fonts are single-byte).

Example:

```cpp
#include <gfx/Gfx.h>

gfx::Target target(display_device);

target.render([](gfx::Surface& s) {
    s.clear(0x0000);
    s.fillRoundRect(8, 8, 200, 60, 10, 0x000F);

    // Transparent text: only the glyph pixels are written.
    s.drawText(20, 20, "Hello", gfx::FONT_8X8, 0xFFFF);

    // Opaque text with a background and a larger glyph.
    gfx::TextStyle style;
    style.font = &gfx::FONT_5X7;
    style.foreground = 0xF800;
    style.background = 0x0000;
    style.opaque = true;
    style.scale = 2;
    s.drawText(20, 90, "BIG", style);
});
```

## Device-agnostic presentation

`Target` is tied to a Tactility display `Device`. The drawing layer is not:

- `Surface(PixelBuffer*)` works on any buffer, including one an app allocates itself.
- Apps that drive a non-display panel (an I2C LCD unit, a headless bitmap, an image export) can
  allocate a `PixelBuffer`, draw into it with `Surface`, and push the bytes themselves.

The generic presentation path is implemented by `SinkTarget`, which is not tied to a display
`Device`:

- `Sink` has `begin(SinkInfo)`, `present(PixelBuffer&, Rect)`, `end()` and an optional `refresh()`.
  `frame_buffer_policy()` lets a backend ask for DMA-capable memory.
- `SinkTarget` owns one full-frame `PixelBuffer`, exposes the usual `Info`/`Stats` and render
  callback, and presents dirty regions to the sink. The buffer persists between frames.
- `DisplaySink` adapts a Tactility display device (whole-row `display_draw_bitmap()`); `gfx::Target`
  remains the optimized display path with flip/banded/PPA modes.
- `UnitLcdSink` adapts an M5Stack LCD Unit over I2C; it lives in `Libraries/UnitLcdGfx`, outside
  `M5UnitModules`. It takes an origin, so it can present a sub-frame while static chrome is drawn
  with the unit's own primitives; smaller frames fit fragmented heaps on no-PSRAM boards.

## Phases

Order: **3 (fonts/layout) → 4 (presentation sinks) → 5 (pixel_buffer helpers)**. Steps 3 and 4 stay
inside `Gfx`/new Gfx-side code; step 5 is deliberately last because it changes
`TactilityWork/Modules/graphics-module` and requires a firmware rebuild. The helper set in step 5 is
a proposal to discuss with ByteWelder before starting it. Fonts/layout goes first because it is
self-contained and improves every screen; sinks follow because they are the last Gfx-only
architectural change and their present path can inform the pixel_buffer discussion.

### Step 1 - foundation (done)

- Public `Surface(PixelBuffer*)` and sub-view constructor.
- Bounds + clip rectangle.
- `hline`, `vline`, `drawRect`, `fillCircle`, `drawCircle`, `fillEllipse`, `drawEllipse`,
  `fillRoundRect`, `drawRoundRect`, `fillTriangle`, `drawTriangle`, `fillArc`, `drawArc`,
  `gradient`.
- `Font`, `TextStyle`, `drawText`, `measureText`, 5x7 and 8x8 fonts.
- Improved dirty-region merging.
- Updated README.

### Step 2 - images and composition (done)

- `blit()` with clipping and format conversion, plus a same-format row-copy fast path.
- `blitTransparent()` for colour-key transparency.
- `drawMask()` for 1bpp icon masks (MSB- or LSB-first) and `drawAlphaMask()` for 8bpp alpha masks.
- `Canvas`: an owned `PixelBuffer` + `Surface` for off-screen composition and sprites.
- `blendPixel()` for RGB565 alpha blending.
- `drawPolygon()` / `fillPolygon()` (even-odd scanline fill, up to 32 vertices).
- The existing Material Symbols data in apps is 2bpp LVGL font data rather than 1bpp masks, so a
  2bpp glyph decoder remains a future addition; the 1bpp/8bpp paths are independent of it.

### Step 3 - more fonts and layout (done)

- Native 8x16 Terminus font (`FONT_8X16`), Latin-1 0x20-0xFF, OFL-1.1 (license in
  `Licenses/Terminus-OFL.txt`). `scale = 2` gives a 16x32 cell for large display text.
- Optional per-glyph widths and advances on `Font`, plus the built-in `FONT_5X7_PROPORTIONAL`.
- Configurable tab width (`TextStyle::tabSize`), ellipsis truncation for boxed text
  (`TextStyle::ellipsis`), and wrap/measurement that understands proportional advances.
- A native 12x16 font is deferred until a suitably licensed bitmap dataset is chosen; 8x16 at
  scale 2 covers the 16x32 case for now.
- 9-slice panels, progress bars and gauges stay app-level for now; `Canvas`/`blit` provide the
  building blocks if they move into the library later.

### Step 4 - presentation sinks (done)

- `Sink`/`SinkInfo`/`SinkTarget`: a display-independent target that owns one full-frame
  `PixelBuffer`, tracks dirty regions, and presents them to a backend. The buffer persists between
  frames; `Prepare` is accepted for API symmetry but has no effect.
- `Sink::frame_buffer_policy()` lets backends request DMA-capable memory instead of the default
  external-RAM preference (used by `DisplaySink` when the panel may DMA directly).
- `DisplaySink` adapter: presents whole rows through `display_draw_bitmap()`, valid for every
  display format including monochrome. `gfx::Target` remains the optimized display path
  (flip/full-frame/banded/PPA transform). `Target::release()` frees its render buffers while the
  panel keeps the last frame, for apps that need the memory for another target.
- `UnitLcdSink` in `Libraries/UnitLcdGfx`, outside `M5UnitModules`: sets the LCD window and streams
  changed rows with `UnitLcd::writePixels()`.
- Optional LVGL canvas bridge remains future work.

### Step 5 - pixel_buffer helpers (graphics-module, deferred)

Deferred until after a discussion with ByteWelder. It requires a change to
`TactilityWork/Modules/graphics-module` and its module symbol table, so it needs a firmware rebuild;
`Surface` keeps its own implementations until then. Proposed helpers:

- `pixel_buffer_fill_rect()`
- `pixel_buffer_hline()` / `pixel_buffer_vline()`
- clipped `pixel_buffer_blit()`
- `pixel_buffer_wrap_offset()` for sub-rects without pointer arithmetic
- `pixel_buffer_copy()` for same-format rect copies

Points to settle with ByteWelder before starting it:

- Which of these are shared enough to belong in the module rather than `Gfx`?
- Is adding entries to the module symbol table acceptable, and is there a version/ABI concern?
- Should `pixel_buffer_blit()` become clipped by default (behavior change) or gain a new variant?
- Should sub-view/offset support live in `pixel_buffer` or stay a `Surface` concern?
- Which changes also help the LVGL path, so both consumers benefit from the module work?

## Relationship to `pixel_buffer`

ByteWelder's suggestion ("maybe `Surface.h` should wrap `pixel_buffer`") is already true
internally: `Surface` stores the buffer pointer and uses `pixel_buffer_get_*` for data, stride,
width, height and format. The change in step 1 is to make that relationship public and to stop
requiring a `Target` to create a `Surface`.

The remaining question is how much to push down into `pixel_buffer`. My recommendation is to add
only operations that are useful to more than one consumer (fill, spans, clipped blit, sub-views)
and keep composition (circles, text, arcs) in `Gfx`. That keeps the module small and avoids
rebuilding the module every time a drawing primitive is added. This is the step 5 proposal; it is
deferred until after a discussion with ByteWelder and is not part of the current Gfx-only work.

## Performance notes

- `fillRect` on RGB565/BGR565 uses `std::fill` per row after converting the color once.
- Compound shapes draw horizontal spans, so they touch each row once.
- Text draws runs of set bits, so a glyph row costs one span per run rather than one call per pixel.
- Same-format blits copy whole rows with `memcpy`; transparent blits over a fast RGB565 format
  compare native pixels directly. Cross-format blits go through RGB565 conversion per pixel.
- Polygons mark their bounding box once and fill each scanline with spans.
- Arcs are the expensive primitive: filled arcs test pixels in the bounding box. They are fine for
  small displays and indicators, not for full-screen animation. A scanline arc is a future
  optimisation.
- In banded mode the callback runs once per band. Do not mutate app state in it.
- In flip/full-frame mode the buffer persists. Clear a region before drawing into it again, or use
  `Prepare::Overwrite` and make the callback repaint everything it changes.

### Symbol export notes

Apps are loaded against the firmware's export table, so a missing libc/libm symbol fails at load
time. `Scripts/check-app-symbols.py` reports these. Current workarounds that should be replaced when
the firmware exports the real symbol (or accepted permanently if not worth a firmware change):

- `Polygon.cpp` uses a local `roundToInt()` because `lroundf` (from `std::lround(float)`) is not
  exported. Replace with `std::lround()`/`lroundf()` once `lroundf` is in the c-symbols module.
- App ELFs do not define `__dso_handle`, so function-local static objects with destructors cannot be
  used (a `static gfx::Canvas` produced an undefined `__dso_handle` at link time). The benchmark
  creates the sprite `Canvas` as an automatic object in `runApplication()` and passes it through
  `Context` instead. This is a linkage limitation, not a missing export-table entry, but worth
  knowing when adding assets to direct-rendering apps.
- App-local display drivers need the firmware to export ESP-IDF's SPI entry points. Linking IDF
  driver archives (`ELF_COMPONENTS`) into the app ELF does not work: their objects reference
  peripheral base addresses that only the firmware's linker script defines (`GPIO`, `GPSPI2`,
  `IO_MUX`, ...), so the load fails on those data symbols. `platform-esp32` now exports
  `spi_bus_initialize`, `spi_bus_free`, `spi_bus_add_device`, `spi_bus_remove_device`,
  `spi_device_polling_transmit`, `spi_device_acquire_bus` and `spi_device_release_bus`; see
  `Apps/MystifySpiDemo/ili9341-panel-module`, which also contains the driver itself.
- App code must not be called from an ISR that may run with caches disabled: app ELFs can be
  loaded into PSRAM (`CONFIG_ELF_LOADER_LOAD_PSRAM`), while the firmware builds with
  `CONFIG_SPI_MASTER_ISR_IN_IRAM`. That is why the ILI9341 driver uses synchronous
  `spi_device_polling_transmit()` transactions and no completion callback; an `esp_lcd`-based
  driver needs a completion signal that does not execute app code in that window.

`std::cos`/`std::sin` (double), `memcpy`, `snprintf`, `operator new` and the graphics-module calls
all resolve against the current firmware.

## File map

```
Include/gfx/
  Gfx.h            umbrella header
  Rect.h           rectangle/point maths
  DirtyRegions.h   damage set
  Surface.h        drawing surface
  Canvas.h         owned PixelBuffer + Surface
  Sink.h           Sink interface and SinkTarget
  DisplaySink.h    display_draw_bitmap() sink adapter
  Font.h           font descriptor, text styles, font declarations
  Text.h           text measurement helpers
  Target.h         display presentation policy
Source/
  Surface.cpp      core, clipping, rects, lines, gradients
  Shapes.cpp       circles, ellipses, rounded rects, triangles, arcs
  Images.cpp       blit, transparency, masks, alpha blending
  Polygon.cpp      polygon outline and scanline fill
  Text.cpp         text layout and rendering
  Fonts.cpp        built-in 5x7/8x8/proportional data and measurement
  FontTerminus16.cpp  8x16 Terminus font data (OFL-1.1)
  Canvas.cpp       canvas allocation
  SinkTarget.cpp   generic sink presentation and dirty regions
  DisplaySink.cpp  display device sink adapter
  ColorUtils.h     internal colour/format helpers
  Target.cpp       display presentation
Licenses/
  Terminus-OFL.txt OFL-1.1 text for the 8x16 font data
```

`Libraries/UnitLcdGfx` contains the separate `UnitLcdSink` adapter; it is not part of the Gfx
library and does not modify `M5UnitModules`.

Apps include `Libraries/Gfx/Include` and compile `Libraries/Gfx/Source/*.c*` through their existing
CMake `GLOB`; there is no separate library build.
