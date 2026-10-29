# Measurements

All on a LilyGO T-Deck (ESP32-S3, 8 MB PSRAM), Tactility 0e276da2, SD card over SPI.
Test area: central Budapest, OpenFreeMap planet 20261025. The z14 tiles there are among the densest
(227-356 KB compressed, 380-610 KB inflated, 1300-1700 drawn features per tile).

## M1 (2026-10-29), synchronous rendering

| What | Result |
|---|---|
| LVGL stop / start | 186-238 ms / 806-1067 ms |
| Read one compressed z14 tile from SD | 460-720 ms (about 0.5 MB/s) |
| Render one z14 tile (scan + 3 sweeps) | 750-930 ms (scan 65-100, geometry 360-430, fill 110-130) |
| First z14 screen, 4 tiles, nothing cached | 5.6 s |
| z13 screen, 6 tiles, nothing cached | 3.8 s |
| Overzoomed z15 / z16 screen from z14 data | 4.7 s / 4.6 s |
| Screen from already rendered tiles (pan, zoom back) | 28 ms (compose 4 ms, overlay and display 24 ms) |
| Free memory while the map runs | internal ~117 KB (largest block 31 KB), PSRAM ~4.5 MB with 24 cached tiles |

Steps that made the difference: the tinfl decompressor in internal RAM instead of PSRAM (scan 580 -> 70 ms),
one read of the compressed file instead of one per pass, filling only the touched columns of each row
(fill 370 -> 120 ms), and no round joins on thin or straight parts of lines.
On the PC the same tiles render in 11-20 ms each.

## M2 (2026-10-29), interactive map

Two render lanes (one task per core), PSRAM cache of 24 tiles, SD card cache with run-length coded tiles
(a dense z14 tile codes to 75 % of 128 KB), cache files written only when no visible tile waits.

| What | Result |
|---|---|
| Pan or zoom over tiles in the PSRAM cache | 35-55 ms per frame |
| z14 screen from the SD card cache (4 tiles) | 1.0 s |
| z14 screen, nothing cached (4 dense tiles) | 5.0 s |
| z15 screen overzoomed from z14, nothing cached | 3.5-4.8 s |
| Read one cached tile from SD | 410-480 ms alone, up to 2.9 s while another lane writes |
| SD card cache write of one tile | 0.5-1.5 s |
| Free memory while the map runs | internal ~64 KB (largest block 31 KB), PSRAM ~2.8-3.2 MB |

The SD card over SPI (0.25-0.5 MB/s) is the limit for cold and warm starts, rendering a dense tile takes
0.5-0.9 s per lane. Touch gestures (drag, pinch, double tap, long press) are written but not tested yet,
only keys were driven through the debug script.

## M3 (2026-10-29), download on the device

| What | Result |
|---|---|
| TileJSON request and parse | about 1.5 s after "Connecting" |
| Small test area, Szeged centre (20 tiles, z0-14) | 1326 KB in 18 s, about 74 KB/s with two connections |
| City frame, Szeged (10.6 x 6.9 km, 74 tiles, z0-14) | complete, 1.1 MB new (20 tiles shared with the first area) |
| Skipping tiles that are already on the card | about 1 s per tile while the map renders (SD card contention) |
| Free internal RAM during a download | about 42-50 KB (mbedTLS buffers are in PSRAM) |

Size estimate table: mean of 290 real tiles (Budapest, Vienna, London, Split, rural Hungary). Dense city
z14 tiles are 150-260 KB, so the 50 MB limit is reached at roughly 200-370 z14 tiles, long before 3000 tiles.
The downloader stops at 50 MB of real data too, in case the estimate was low.

Notes:
- Tactility's certificate bundle rejects tiles.openfreemap.org: the server sends GTS Root R4 cross-signed by
  GlobalSign Root CA, and the bundle check only looks at the top certificate of the received chain. The app
  passes its own roots (GTS R1/R4, ISRG X1/X2) as cert_pem instead.
- The download speed was not measured in detail yet (the serial log was lost in the city run).

## M3.1 (2026-10-29), host numbers only

No device in this session: everything below was measured on the PC (Windows, MSVC Release build), on 9 dense
z14 tiles of central Budapest (14/9058-9060/5728-5730, OpenFreeMap planet 20261025, 162-365 KB each).
The PC renders about 50-80 times faster than the T-Deck, so only the ratios carry over. Device numbers are
pending until the T-Deck is back (test list in HANDOFF.md).

**Prepared tiles (`.prt`) against the originals (`.pbf`), `host/prepare_tiles`:**

| What | Result |
|---|---|
| Size, raw `.prt` / `.pbf` | 22.4 % (mean 52 KB against 233 KB, per tile 18.8-30.1 %) |
| Size, deflated `.prt` / `.pbf` | 16.1 % (mean 37 KB, per tile 13.5-21.2 %) |
| Features kept | 94-98 % (only what the style draws at z14-z19, smaller than 1 px at z16 dropped) |
| Points kept after 0.5 px (z16) simplification | 89-96 % |
| Render z14 tile, host: `.pbf` / raw `.prt` / deflated `.prt` | 11.0 / 5.1 / 5.2 ms |
| Render z16 overzoom, host: `.pbf` / raw `.prt` / deflated `.prt` | 7.1 / 1.1 / 1.6 ms |
| Conversion of one tile, host | 4.4-4.6 ms (raw and deflated, including tdefl) |
| Pixel difference `.prt` against `.pbf`, z14 | mean 1.4-1.6 of 255, at most 0.02 % of pixels off by more than 64 |
| Pixel difference at z16 / z18 (overzoom, 1024 units per tile) | z16 mean 2.5, 0.63 % strong; z18 mean 2.2, 1.85 % strong |

On the device the read time should shrink with the size: at the measured 0.5 MB/s a 233 KB `.pbf` took
0.46-0.72 s to read, a 37-52 KB `.prt` should take about 0.07-0.1 s. Whether raw or deflated is faster on the
device (smaller read against an extra inflate of about 200 KB) is the device test's question, the setting
"Prepared tiles" switches between them.

**Cheaper buildings at native z14** (no anti-aliasing, buildings under 2 px left out), per tile, host:

| What | Before | After |
|---|---|---|
| Geometry phase, from `.pbf` | 4.60 ms | 4.78 ms |
| Buildings (their geometry and fill), from `.pbf` | 1.86 ms | 1.85 ms |
| Fill phase, from `.pbf` | 1.44 ms | 1.33 ms |
| Geometry phase, from raw `.prt` | 3.85 ms | 4.01 ms |

On the PC the change is within noise: the anti-aliased coverage maths is cheap on a desktop FPU, and few
central Budapest buildings are under 2 px. The ESP32-S3 pays more for the per-cell float work it skips, so
the device number decides (pending).

| Device numbers | Status |
|---|---|
| Cold first z14 screen (was 5.0 s) | pending |
| `.prt` against `.pbf` size on the card | pending (host ratio above) |
| `.prt` raw against deflated, read and render time | pending |
| SD raster cache read against rendering from `.prt` (decides whether the SD cache stays) | pending |
| Geometry time with cheaper buildings | pending |
| Worst single tile read while panning (was 2.9 s) | pending |
| Free internal RAM with the new I/O task (6 KB stack) | pending, was ~64 KB while the map runs |

## M3.1 on the T-Deck (2026-10-29, evening)

Tactility v0.8.0-dev, same SD card, central Budapest (budapest region, deleted after these runs) and ESA ESTEC.
Logs come from new log lines: "Slow loop", "Slow frame" (lock, compose, overlays, display), "Slow I/O job",
"Stack left".

**What caused the freezes.** With the first M3.1 build, single frames took 1.4-3.3 s while tiles loaded, and the
render lane on core 0 drew a tile up to 8 times slower than the one on core 1 (3,047 ms against ~400 ms).
Three causes, all fixed:
- the I/O task ran above the app and the lanes, pinned to core 0, and the SD card driver keeps the CPU busy;
- the SD raster cache wrote every rendered tile (0.2-0.9 s each) while the user panned, and on the T-Deck the
  display shares the SPI bus with the card, so the frame waited for the write;
- the app's loop had the same priority as two lanes and the I/O task, so it got half a core while tiles loaded.
Later runs also showed slow FAT work: tile lookups in every region folder, a folder scan reading many headers in
one job, deleting 6 cache files per step, and storing downloaded tiles while panning.

| What | First M3.1 build | Now |
|---|---|---|
| Cold first z14 screen, central Budapest (PSRAM empty) | 5.0 s in M2 | 1.8-2.0 s |
| Cold first z14 screen at ESTEC | | 1.7 s |
| Worst frame while panning | 3.3 s | about 0.3 s |
| Tile read while panning, median (fast pan over 50-110 tiles) | 353 ms | 131-176 ms |
| Worst single tile read while panning | 2.9 s in M2, 1.1 s | 0.25-0.55 s |
| Render of a dense z14 tile from `.prt` | 300-3,047 ms | 300-700 ms (geometry 270-560 ms) |
| Frame while tiles load | | 120-140 ms (36 ms when nothing loads) |

**`.prt` against `.pbf` on the card** (tiles 14/9058/5727-5731): `.pbf` 992 KB, deflated `.prt` 173 KB, 17.5 %.
The densest tile: 232,636 bytes against 33,646 (14.5 %).

**Raw against deflated `.prt`**, the same 9 central tiles and view:

| | raw | deflated |
|---|---|---|
| Read per tile | 157-346 ms | 93-288 ms |
| Inflate | 0 | 11-17 ms |
| Render | 529-678 ms | 544-704 ms |
| First z14 view | 1.84 s | 1.81 s |

Deflated stays the default: 30 % smaller and a little faster.

**SD raster cache against rendering from `.prt`.** A cached tile read in 186-580 ms against about 100-150 ms
read plus 300-700 ms render from `.prt`, so the cache saves 0.3-0.5 s per tile on a free core, but it wrote
0.2-0.9 s per tile while the user panned (the freezes) and had grown to 1,122 tiles and 41.9 MB. It is now off by
default; switched off, idle time deletes it (2 files per step). The setting stays.

**Other numbers**

| What | Result |
|---|---|
| Free memory, map ready | internal 66 KB, PSRAM 2.0 MB (PSRAM tile cache now 32 tiles, 4 MB) |
| Free memory while tiles render | internal ~51 KB (largest block 31 KB) |
| Stack left, of 8 KB each | app loop 4.8 KB, I/O task 3.6 KB, render lanes 5.6 KB |
| Store one downloaded tile (folders, create, write, rename) | 0.4-0.7 s, now only while the map is left alone |
| Delete the budapest region through the app | 30 s |
| First HTTPS connection after boot | a 1.6-2.0 s pause of all app tasks (also plain drawing code), not on later connections; outside the app, now placed after the first view |

Not measured on the device yet: cheaper buildings (no A/B run), memory during an area download.

## M3.2 (2026-10-29), proof-of-concept release

**Prepared tile precision** (host, `host/prepare_tiles --zoom <z>`, the same 9 dense central Budapest z14 tiles).
With 1024 units per tile a unit is 4 px at z18, and the 0.5 px (at z16) simplification moves points by up to 2 px
at z18, so lines looked stepped at z17-z19. Pixel difference is against rendering the `.pbf` itself, worst tile.

| Units, simplification | Deflated `.prt` / `.pbf` | Raw | Render z14 / z18, raw | z14 diff (mean, strong) | z18 diff (mean, strong) |
|---|---|---|---|---|---|
| 1024, 0.5 px at z16 (before) | 16.1 % | 22.4 % | 5.3 / 1.0 ms | 1.63, 0.02 % | 2.24, 1.85 % |
| 2048, 0.5 px at z16 | 18.3 % | 23.3 % | 5.2 / 0.9 ms | 1.11, 0.01 % | 2.16, 1.75 % |
| **2048, 0.25 px at z16 (chosen)** | **19.0 %** | **24.3 %** | **5.6 / 0.8 ms** | **1.01, 0.00 %** | **1.75, 0.56 %** |
| 4096, 0.5 px at z16 | 20.9 % | 24.8 % | 5.1 / 0.8 ms | 0.30, 0.00 % | 1.56, 1.45 % |
| 4096, 0.125 px at z16 | 21.8 % | 26.0 % | 5.3 / 1.0 ms | 0.01, 0.00 % | 0.11, 0.00 % |

Coordinates were already delta-coded zigzag varints, so only the precision changed. 4096 units grow the deflated
files (the default kind) by 30-35 %, over the 25 % budget; 2048 units with a matching simplification (1 px at z18)
grow them by 18 % (mean 37 -> 43 KB), render time stays within noise, and strongly differing pixels at z18 fall
from 1.85 % to 0.56 %. Prepared format version 2: version 1 tiles on the card are redrawn from the `.pbf` and
converted again in idle time. The unit test now renders the fixture tile at z18 (golden-18) and compares the
prepared copy with it: 1024 units fail it (mean 3.18, 2.99 % strong), 2048 pass (1.14, 0.43 %).

**On the T-Deck (M3.2, developer option `dev_probe=1`: per-second memory, per-core stall probes, task states).**

| What | Before the fixes | After |
|---|---|---|
| Lowest free internal RAM, download + panning (~4 min) | 183 bytes (largest block 112) | 19.3 KB (largest 9.7 KB) |
| Lowest free PSRAM, same run | 60 KB | 301 KB |
| Failed display transfers ("spi transmit color failed") | 352 | 0 |
| Render lane stack left (tdefl during conversion) | 600 of 8 KB | 2.6 of 10 KB |

Causes: display strips in PSRAM made the SPI driver allocate a 15 KB internal DMA copy per strip (failed when RAM was
short: the flicker during downloads); network task stacks (28 KB) and three TLS clients in internal RAM; download
bodies kept at their largest size; two conversions at once (~0.4 MB PSRAM each); 32 cached tiles.
Fixes: internal DMA strips of 12 rows, network task stacks in PSRAM on core 1, bodies freed after use, one conversion at
a time, 28 cached tiles, 10 KB lane stacks.

**Cheaper buildings at native z14, A/B** (central Budapest, 4 views, fresh start each): geometry phase from `.prt`
388 ms per tile with cheaper buildings (18 tiles) against 408 ms without (24 tiles), about 5 % less. First-view times
(0.75-4.0 s) vary more with what the card is doing than with this change. Kept.

**First HTTPS connection** (see docs/upstream/https-stall.md): DNS and TCP 21-43 ms, TLS handshake 2.0-3.6 s (12.6 s
once while the card was busy). On core 0 it held the map loop and render lane there for its whole length; on core 1 no
stall was seen. Network tasks now run on core 1.

**Monochrome palette**, forced on the T-Deck (`dev_mono=1`) and thresholded like Tactility's 1-bit conversion
(luma >= 128 is white): menus and text read well, but most of the map disappears in 1-bit: buildings, parks and wood
are light greys above the threshold, and anti-aliased thin casings turn partly white. Greyscale displays are fine.
A 1-bit style (no anti-aliasing, dark fills or outlines) is M4 work.

Not measured: location switched off and on (needs the user), the 30-minute soak.
