# Gfx Benchmark

A PDQ-style primitive benchmark for the direct-to-display `Libraries/Gfx` stack. It is modelled on
the LCD GFX test in `M5UnitTest`, but draws through `gfx::Target` so it runs on any display the
Gfx library supports: MIPI DSI, RGB, SPI, e-paper-class mono/grayscale panels, hardware frame
buffers, full-frame buffers, and banded rendering.

## Phases

The app runs the same phases as the LCD GFX test, plus two extra phases for the image and polygon
paths, adapting sizes and step counts to the display so a 135x240 panel and a 720x1280 panel both
finish in a reasonable time:

1. Screen fill
2. Text (5x7, proportional 5x7, 8x8, 8x16, scaling, wrapping)
3. Pixels
4. Lines (four corners, two sweeps each)
5. H/V lines
6. Rectangles (filled)
7. Rectangles (outline)
8. Triangles (filled)
9. Triangles (outline)
10. Circles (filled)
11. Circles (outline)
12. Arcs (filled)
13. Arcs (outline)
14. Round rects (filled)
15. Round rects (outline)
16. Sprites (Canvas, colour-key blit, alpha mask)
17. Polygons (filled star, outline pentagon)

Every phase starts with a clear and runs with `Prepare::Overwrite`, so the benchmark is not
affected by leftovers in a flip back buffer. After each phase has been measured, it is held on
screen for 250 ms so it is visible; that delay is not included in the results.

## Results

After the last phase the app draws an on-screen table:

- Phase name and draw time in microseconds, right-aligned.
- Total draw and present time in the footer.
- Render mode and resolution in the subtitle.
- Short landscape panels (Cardputer, T-Embed) switch to a two-column table, so all phases and the
  full footer fit.
- If the table still doesn't fit, it shows the slowest phases that do.
- On very narrow displays (under 120 px) a compact summary shows the total and the slowest phase.

The same numbers are logged to the serial console with draw, present, total and dirty-pixel counts.
This makes it easy to compare render modes and panels without reading the display.

## Controls

- Any input cancels the run or exits the results screen.
- The app also honours `APP_EVENT_CLOSE`, so the app manager can stop it normally.

## Build and run

```sh
tt app build --path Apps/GfxBenchmark --architecture esp32s3
tt app bir   --path Apps/GfxBenchmark --architecture esp32s3 --host <device>
```

Replace the architecture with `esp32`, `esp32c6` or `esp32p4` as needed. The app targets every
platform in its manifest.

## Slow-refresh displays

E-paper and other slow-refresh panels skip the benchmark and show a message instead. Running the
full PDQ workload on a panel that needs seconds per refresh would not produce meaningful numbers.
The message is drawn with the same Gfx text path and followed by a full-quality refresh.
