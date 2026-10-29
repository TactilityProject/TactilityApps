# Tactility Maps — Project Plan

Oct 28, 2026 · @Adam

## Goals and constraints

Tactility Maps is a standalone, offline-first vector map app, shipped as an external app: the device downloads a region as individual tile files from OpenFreeMap, then renders it fullscreen with LVGL stopped.

- **Standalone, no hosting cost:** the device fetches its own maps over Wi-Fi from [OpenFreeMap](https://openfreemap.org/), a free public tile server. We host nothing.
- **Standard, stable data:** MVT tiles in the [OpenMapTiles schema](https://openmaptiles.org/schema/), stored on SD as one small file per tile (`z/x/y.pbf`).
- **Fullscreen, no LVGL:** as the maintainer suggested, the app stops the LVGL module and draws through the `pixel_buffer` API, like the built-in Terminal app does.
- **External app:** built with the Tactility SDK as an ELF app, not built into the firmware.
- **Hardware:** ESP32-S3 with PSRAM as the main target, and must also run on boards without PSRAM (Cardputer). E-paper refresh is handled by Tactility; the app only switches to a high-contrast palette when the display is monochrome or greyscale.
- **Licence:** Tactility is GPL-3.0, so borrowed code must be GPL-compatible (Apache-2.0, MIT, BSD are fine).

## User experience

The target is a full-quality app on a T-Deck-sized screen (320×240, keyboard, trackball, touch): the map fills the screen, and anything else appears only while it is needed.

**Screen mock-ups (illustrative values):**

```
 Map view                                 Download this area
+------------------------------------+   +------------------------------------+
| (Updating 42%)                     |   |########## dimmed ##################|
|                                    |   |####+------------------------+######|
|        roads / water / land        |   |####|                        |######|
|                ▲                   |   |####|   area to download     |######|
|                ●  (you)            |   |####|                        |######|
|                                    |   |####+------------------------+######|
| © OpenMapTiles © OSM      |200 m|  |   | 1,240 tiles · ~38 MB   [Download]  |
+------------------------------------+   | max zoom 14 · fits the limit       |
 Only marker, scale and attribution       +------------------------------------+
 stay on screen. The update pill shows    The frame stays in the middle, the map
 only while updating.                     moves and zooms under it. Tile count
                                          and size update live; Enter downloads.
```

**Principles**

- **The map is the UI.** No toolbar, no permanent buttons. Always on screen: position marker, scale bar and the attribution, which collapses to a small "©" mark after 5 s (see Decisions).
- **Instant feedback.** Panning moves already-rendered pixels at once; missing tiles show a light placeholder and fill in when ready. Nothing ever waits on the SD card or the network.
- **One short menu** (`M` key, ☰ button or long press): Download area, Regions, Settings, About and licences, Exit. Lists fit on one screen, about 6 rows at 240 px.
- **Quiet background work.** Idle updates show a small pill only while they run.
- **Readable on small screens.** Text at least 12 px (Tactility's IBM Plex Mono from graphics-module), high contrast, monochrome palette when the display needs it.
- **Every action works with all three inputs**, so it also works on Cardputer (keys only) and touch-only boards.

**Controls** (the manual, as the app works since M3.1)

**Map screen.** The map fills the screen. Always visible: your position (●, with an accuracy
circle), the scale bar, and the © mark.

| To… | Keyboard | Trackball | Touch |
| --- | --- | --- | --- |
| Move the map | W A S D | Roll | Drag |
| Zoom in / out | I / O | — | Pinch, double tap to zoom in, or the + / − buttons |
| Show / hide controls | Space | Click on empty map | Tap the map |
| Centre on my position and follow | G | — | ⌖ button |
| Stop following | Move the map | Roll | Drag |
| Menu | M | Long click | ☰ button, or long press |
| Download this area | Shift + D | — | Menu → Download area |
| Credits | — | Click © | Tap ©, tap again for About and licences |
| Leave the app | Esc or Backspace | Menu → Exit | Menu → Exit |

**Controls.** Hidden by default. When shown: ☰ menu top left, + and − in the middle of the right edge, ⌖
position bottom right above the scale bar. With a keyboard each button shows its key. ⌖ is filled while
following, an outline when not, and crossed out when there is no position. With the trackball in mouse
mode a pointer appears while the controls show; roll to a button and click it, click on empty map to hide
them. On boards with buttons only (no keyboard, no touch), the buttons move the focus through the controls
and press the focused one.

**Menu:** Download area, Regions, Settings, About and licences, Exit. W/S, trackball or finger to
move; Enter, click or tap to choose; Esc, Backspace or tap outside to close.

**Downloading:** open Download area. The frame stays in the middle; move and zoom the map under
it. The bar shows tiles, size and max zoom, and updates live. Enter, click or Download starts it;
Esc cancels. It downloads in the background and resumes after a restart; areas share tiles.

**Regions:** list with size, date and status (complete, downloading, outdated). Select with W/S,
trackball or tap; delete with Del then confirm, or long press then Delete; Esc goes back.
Deleting keeps tiles another region still uses.

**Browsing:** with Wi-Fi, tiles on screen that have no detail on the card yet are downloaded while you
look at them (a small "Downloading map" label shows) and kept in the region "browsed", so they work
offline later. Without Wi-Fi a short message says that detail is missing here. Only the tiles on screen
are fetched, one at a time.

**Settings:** SD card cache on or off, prepared tiles deflated or raw, smooth pinch on or off, download
while browsing on or off. Defaults: SD card cache off (switched off, its files are deleted in idle time),
deflated, on, on. The map opens at ESA ESTEC, Noordwijk.

**Updates:** after about 30 s without input, outdated regions update quietly and a small
"Updating 42%" label shows. Any input pauses it. Only on Wi-Fi, and only when charging or above
a battery level. *(Planned for M4, not in this version yet.)*

**Position:** comes from Tactility's location service (GPS or approximate). If it's off, the ⌖
button is crossed out and pressing it says how to enable it in Tactility's settings.

The T-Deck keyboard has no arrow keys, which is why panning uses WASD and zoom uses I/O. Lower case d pans, so the download frame opens with Shift+D or from the menu.

## Prior art: what to take from each

The best code base to borrow from is **m5-cardputer-offgrid-tiny-map**: it already streams planet PMTiles + MVT on an ESP32-S3, and it is Apache-2.0, so it is GPL-compatible. OrcMaps and mapnavigation are best used for design ideas.

| Project | Data | Rendering | Licence | What we take |
| --- | --- | --- | --- | --- |
| [m5-cardputer-offgrid-tiny-map](https://github.com/yuiseki/m5-cardputer-offgrid-tiny-map) | Planet PMTiles + MVT on SD, z0–14 | Streaming decode, each tile rendered once and cached on SD as RGB565 | Apache-2.0 | Code: streaming inflate, streaming MVT decoder, tile cache. Works with no PSRAM |
| [OrcMaps](https://github.com/hardcoreerik/OrcMaps) | PMTiles v3 + MVT | Decode into a feature model, then style + render | AGPL-3.0 | Ideas only: format decision write-up, pack manifest, style system, measured tile sizes |
| [mapnavigation](https://github.com/chaitanyaasati/mapnavigation) | Custom VT01 tiles over HTTP | Anti-aliased coverage rasteriser, casing then fill | No licence file | Ideas only: rasteriser technique, draw passes, label collision. Code can't be copied without a licence |
| [IceNav-v3](https://github.com/jgauchia/IceNav-v3) + [Tile-Generator](https://github.com/jgauchia/Tile-Generator) | Custom NAV tiles + ROUTE.bin | LovyanGFX | GPL-3.0 | Later: offline A* routing graph format |

**Key numbers from these projects:**

- mapnavigation renders a full screen in 30–120 ms at z10–12 and 350–600 ms at dense z14–16 on an ESP32-S3 with 8 MB PSRAM.
- The Cardputer project's first render of a Tokyo view took about 16 s, then about 0.3 s once tiles were cached. That is with no PSRAM and gzip inflated on the fly.
- OrcMaps measured busy z14 tiles at up to 142 KB decompressed; decoding into a full feature model grew one tile to about 1.2 MiB. So we should draw while decoding, not build a feature tree.
- mapnavigation tried LVGL's ThorVG backend and dropped it (about 1 ms and 8 KB heap per polyline). Another reason to bypass LVGL.

**Voice commands** in mapnavigation are cloud-based (Groq Whisper + a hosted LLM). Only the final place lookup runs on the device. Out of scope for now.

## Architecture

The map view is a streaming pipeline that never holds a whole tile in RAM; the rendered-tile cache is what makes panning fast.

**Pipeline (tiles are downloaded once, rendered once, then served from cache):**

```
DOWNLOAD (Wi-Fi, normal LVGL screen)
  [OpenFreeMap]                --> [Region downloader]              --> [SD card]
   free, OpenMapTiles schema        one GET per tile, keep-alive         maps/region/z/x/y.pbf
   ≤3,000 tiles per download        max 4 parallel, own User-Agent       + rendered tile cache
                                                                             |
MAP VIEW (fullscreen, LVGL stopped)                                          v
  [MVT decoder]               <-- [Streaming inflate]              <-- [Tile file reader]
   streams one ring at a time      tinfl, 32 KB window                   opens z/x/y.pbf by path
   layer, class, geometry          no whole tile in RAM                  overzooms past z14
        |
        v
  [Rasteriser]                --> [TILE CACHE]                     --> [Compositor]
   RGB565, coverage AA             PSRAM LRU, then SD                     viewport, GPS marker
   fill, casing, line passes       render once, reuse                     touch, keys, location_get
                                                                             |
                                                                             v
                                                                       [Display]
                                                                        pixel_buffer_blit to panel
                                                                        display_draw_bitmap
```

On a cache hit the compositor reads straight from the tile cache; only the first view of an area runs the full decode. A rendered 256×256 RGB565 tile is 128 KB, so PSRAM holds a few dozen before older ones spill to SD.

**Code layout (one ESP-IDF component, testable on a PC):**

- `tiles/`: tile maths (lat/lon to z/x/y), tile file paths, downloader (OpenFreeMap rules built in)
- `mvt/`: streaming gzip + protobuf reader and geometry command decoder
- `render/`: rasteriser, style table (colour and monochrome palettes), tile cache
- `app/`: Tactility glue (LVGL stop/start, display, touch, keys, location, region picker UI)
- `host/`: PC build that renders a tile to PNG, for tests and style tuning without hardware

## Tactility integration

Everything the app needs is already exported to external apps; the built-in Terminal app (`Tactility/Source/app/terminal/main.cpp`) is the template for a fullscreen, no-LVGL app.

| Need | Tactility API | Notes |
| --- | --- | --- |
| Take over the screen | `module_stop(&lvgl_module)` on start, `module_start(&lvgl_module)` on exit | Exactly what Terminal's `runFullscreen()` does |
| Find the display | `device_get_first_active_by_type(&DISPLAY_TYPE, …)` | Then `display_get_resolution_x/y`, `display_get_color_format` |
| Pick the palette | `display_get_color_format()` | `MONOCHROME` or `GRAYSCALE8` → high-contrast palette; e-paper refresh itself is Tactility's job |
| Draw | `pixel_buffer_create` (PSRAM, 64-byte aligned), `pixel_buffer_get_row`, `pixel_buffer_blit` | Render in RGB565; `blit` converts to the panel's format |
| Push to panel | `display_draw_bitmap(…)` per dirty region, or `display_get_frame_buffer` + `pixel_buffer_wrap` when the driver has 2 hardware buffers | Send a full frame when `DISPLAY_CAPABILITY_REQUIRES_FULL_FRAME` is set |
| Touch | `device_get_first_active_by_type(&TOUCH_TYPE, …)`, `touch_read_data`, `touch_get_touched_points` | Terminal's `TouchInput.cpp` shows the pattern; we add gesture detection |
| Keys | `keyboard_subscribe` / `keyboard_poll` | WASD pan, I/O zoom (see Controls) |
| Position | `location_get`, `location_subscribe` (`struct Location`: lat, lon, accuracy, course, speed) | GPS is set up in Tactility's own GPS settings, so the app needn't touch UART/NMEA |
| Download | `esp_http_client_*` (exported by http-module) | Needed over `http_download_start`, which can't set a User-Agent or reuse one TLS connection across tiles |
| Inflate | `tinfl_decompress` (exported by platform-esp32) | Vector tiles are usually stored gzip-compressed; check the first two bytes and inflate if needed (32 KB window) |
| Storage | SD card, FAT only (FatFs with long names; exFAT is not enabled) | See "Tile storage on FAT" below |

**Before exiting fullscreen** the app must restore LVGL even on error paths, otherwise the device is left with a dead UI. Terminal guards this with a `lvgl_active` flag.

**Map, area frame and small overlays** (pill, scale bar, toasts) are drawn by the app itself, so they match the map and need no LVGL. **Menus, the Regions screen, settings and About** are drawn by the app too, with a small toolkit (list, button, toast, confirm dialog): restarting LVGL took 0.8-1.1 s in M1, too slow for opening a menu, so LVGL stays stopped while the app runs (see Decisions).

## Map data source

Use OpenFreeMap's public tile server: it is free, needs no API key, and its terms explicitly allow offline downloads in apps, so we host nothing.

**OpenFreeMap's rules for offline downloads** ([terms](https://openfreemap.org/tos/), [quick start](https://openfreemap.org/quick_start/)):

- At most 3,000 tiles or 50 MB per download. Larger downloads need permission (info@openfreemap.org).
- At most 4 parallel requests.
- A unique User-Agent identifying the app, e.g. `TactilityMaps/0.1 (+project URL)`.

**What 3,000 tiles covers.** At z14 a tile is about 2.4 km wide at the equator and less further north, so a city of 20 × 20 km is a few hundred tiles across all zooms. A whole country at z14 is far over the limit. The region picker should show the tile count and lower the max zoom (z12 is 16× fewer tiles than z14) until the region fits, rather than splitting one area into several downloads to dodge the limit.

**Why OpenMapTiles is the stable choice.** It is a versioned, documented schema that has been in wide use for years, and OpenFreeMap builds its tiles with it. We map its layers (`water`, `landuse`, `transportation`, `building`, `place`, …) through one style table, so a later schema change touches one file.

**Don't use** openstreetmap.org tile servers for downloads: their [policy](https://operations.osmfoundation.org/policies/vector/) forbids downloading tiles in advance.

**Attribution:** show "© OpenMapTiles © OpenStreetMap contributors" on the map at all times.

### Tile storage on FAT

Tactility mounts SD cards as FAT only, so we store one file per tile in a `z/x/y` folder tree, which keeps every folder small.

| FAT limit | Value | Effect on us |
| --- | --- | --- |
| Max file size | 4 GB − 1 byte | Irrelevant: a tile is tens of KB |
| Max entries per folder | 65,534 (fewer with long names, which use extra entries) | A `z/x/` folder holds one column of tiles; a city has dozens per folder |
| Folder lookup | Linear scan in FatFs | Small folders keep `fopen` fast |
| Smallest file on disk | One cluster (often 32 KB on large cards) | Small tiles waste space; ~3,000 tiles may waste tens of MB, acceptable |
| Filenames | 8.3 names avoid long-name entries | `maps/<region>/14/9076/5636.pbf` fits 8.3 at every level if region names are ≤ 8 chars |

Rendered-tile cache files (128 KB RGB565 each) use the same `z/x/y` layout under `cache/`.

## Downloading maps

The app offers two ways to pick an area, and both end in the same downloader: a list of tiles fetched within OpenFreeMap's limits.

| Mode | How it works | Strength | Catch |
| --- | --- | --- | --- |
| **Download this area** (Google Maps style, build first) | Frame an area on the map; tile count and size estimate update live as you move or resize it | Exact, fits the 3,000-tile limit naturally | User has to frame the area |
| **Region catalog** (CoMaps style, build second) | Browse continent → country → region → city from a catalog file shipped with the app ([Geofabrik's region index](https://download.geofabrik.de/) as the source) | Familiar, one tap, uses the real outline so sea and neighbours are skipped | Big countries exceed 3,000 tiles at z14, so max zoom drops automatically (e.g. a 50,000 km² country fits to z12, not z13) |

- **Tile count is exact; size is an estimate.** Single tile files don't report their size in advance, so size = tile count × a typical size per zoom, from a table we measure once from real OpenFreeMap tiles.
- **Max zoom is chosen for the user:** the highest zoom that fits the limit, which they can lower.
- **Downloads run in a background task** with progress, pause and resume; the map stays usable.
- **Regions screen:** list of downloaded regions with size, date and update status; delete or update from there.
- **Overlapping regions share tiles.** A tile already on the card is skipped, and deleting a region keeps tiles another region still uses.

### Updates while the map is idle

When the map is open but untouched, the app quietly refreshes outdated regions.

- Starts after about 30 s with no input; any touch, key or trackball movement pauses it at once, so panning never competes for the SD card, CPU or Wi-Fi.
- One request at a time, tiles near the current view first.
- Each region keeps a `meta.json` with the source version and download date. A newer source version marks the region outdated. OpenFreeMap rebuilds weekly; updating monthly is plenty.
- If OpenFreeMap sends ETag or Last-Modified headers, unchanged tiles cost almost nothing to check (to be tested).
- New tiles are written to `.tmp` and renamed into place; the rendered copy is dropped from the cache and redrawn if on screen.
- The TLS connection (about 40 KB of internal RAM) stays open while updating; it is closed before rendering only if free-RAM measurements show we need to.
- Progress is saved, so it resumes the next time the map sits idle, even after a restart.
- Only on Wi-Fi, and only when charging or above a battery threshold.

## Milestones

Build the renderer on a PC first, then put a static map on the device, and only then add downloading; M3 is the point where the app becomes standalone.

| Milestone | Work | Done when |
| --- | --- | --- |
| **M0 · Host prototype** | Streaming inflate, MVT decoder and tile maths in plain C/C++; rasteriser + style table; PC build renders a tile to PNG; unit tests with real OpenFreeMap tiles | PC render of a sample region looks right |
| **M1 · Static map on device** | External app, fullscreen: stop LVGL, draw with pixel_buffer; reads z/x/y tile files copied to SD by hand; measure render time and LVGL stop/start time | One screen at z14 under 1 s (target) |
| **M2 · Interactive map** | Pan and zoom with touch and keys; tile cache in PSRAM + SD; GPS marker from location_get, follow mode; OSM attribution always visible | Panning a cached area feels instant |
| **M3 · Download on device** | Tile downloads from OpenFreeMap, within its limits; "Download this area" frame with live tile count and size; downloader with resume, progress, max 4 parallel | A city downloads with no PC involved |
| **— PoC release —** | Publish, get the Tactility maintainer's feedback, then email OpenFreeMap about larger downloads | |
| **M4 · Polish and more devices** | Region catalog, idle-time updates, monochrome palette; waypoints, track recording, GPX import and export; smooth zoom animation for +/− and double tap (affine compositor, snap to whole zoom levels); test on T-Deck, Cardputer and a monochrome display | Runs on boards with and without PSRAM |
| **M5 · Navigation** | Online routing (OSRM, Valhalla or GraphHopper); route following, next turn, off-route detection; offline routing graph later (see IceNav ROUTE.bin); map rotation on the same compositor: two-finger rotate, compass button that resets to north, heading-up while following using GPS course, larger tile margin for the ~400 px diagonal | Turn-by-turn on a downloaded region |

M0 can start right away and needs no hardware. In the end nothing was ported from the Cardputer project: inflate uses miniz's tinfl directly, and the decoder and rasteriser were written for this project, so no Apache NOTICE is needed. Labels are left for after M5. The proof-of-concept release comes after M3; we publish it, get the Tactility maintainer's feedback, then contact OpenFreeMap about larger downloads before continuing.

## Decisions

**Decided (29 Oct 2026):**

- No paid hosting: tiles come from OpenFreeMap, within its offline-download rules.
- External ELF app, not built into the firmware.
- One file per tile in a `z/x/y` tree on FAT, no single big archive.
- E-paper refresh is Tactility's job; the app only swaps to a monochrome palette.
- Stable schema: OpenMapTiles from OpenFreeMap, the only supported tile source.
- Labels come later.
- **Larger regions:** for the proof of concept, lower max zoom automatically for big areas and let users combine downloads (e.g. a country to z12 plus cities to z14). After the PoC release, once the Tactility maintainer likes it, email OpenFreeMap (info@openfreemap.org) to ask for permission above 3,000 tiles.
- **TLS memory:** an HTTPS connection needs about 40 KB of internal RAM. We measure free RAM while the app runs and close the connection before rendering only if memory turns out short.
- **Attribution on a small screen (29 Oct 2026):** the full "© OpenMapTiles © OpenStreetMap contributors" shows for 5 s when the map opens, then collapses to a small "©" mark in the bottom left corner. Tapping it shows the text again, and the menu has "About and licences". The OSMF attribution guidelines allow collapsing after five seconds as long as the licence stays easy to find, OpenMapTiles asks for a visible credit on the map. Attribution only in settings would not be allowed.
- **Licensing:** copy only from Apache/MIT/BSD/GPL code. mapnavigation has no licence, so we write our own rasteriser using the same published technique (signed-area coverage, as in font-rs and stb_truetype).

**Decided (29 Oct 2026, M3.1):**

- Prepared tiles: a render-ready `.prt` copy next to the untouched original `.pbf`.
- Position comes only from Tactility's location API; the app never changes location settings.
- UI is drawn by the app; LVGL stays stopped while the app runs.
- SD raster cache off by default (device test, 29 Oct 2026): reading a cached tile saved 0.3-0.5 s against drawing it from its `.prt`, but writing the cache (0.2-0.9 s per tile, on the SPI bus the T-Deck's display shares) froze the screen while panning, and it grew to 42 MB. The setting stays; switched off, idle time deletes it. See MEASUREMENTS.md.
- Download while browsing (user request, 29 Oct 2026): with Wi-Fi, the tiles on screen that lack detail are fetched one at a time into the region "browsed"; offline the map says detail is missing. This is normal map viewing in OpenFreeMap's terms (no limit there), it still uses the app's User-Agent and stays within 4 parallel requests (2 for an area download, 1 for browsing). A setting switches it off.
- Zoom and rotation: in the PoC only pinch is smooth (live scaling of cached pixels, snap to a whole zoom level on release). Animated +/− zoom comes in M4, map rotation in M5, both on one affine compositor that samples cached tiles; vectors are not re-rendered per frame. Off on e-paper.

## References

- [Tactility](https://github.com/TactilityProject/Tactility): Terminal app (`Tactility/Source/app/terminal/`), `Modules/graphics-module/include/graphics/pixel_buffer.h`, `TactilityKernel/include/tactility/drivers/display.h`, `Modules/location-module`, `Modules/http-module`, SD mounting in `Platforms/platform-esp32/source/drivers/esp32_sdmmc_fs.cpp`
- [Tactility PR #658: pixel_buffer and graphics-module](https://github.com/TactilityProject/Tactility/pull/658)
- [Tactility PR #660: optional DisplayApi frame buffers](https://github.com/TactilityProject/Tactility/pull/660)
- [OpenFreeMap terms of service](https://openfreemap.org/tos/) and [quick start](https://openfreemap.org/quick_start/)
- [OpenMapTiles schema](https://openmaptiles.org/schema/)
- [m5-cardputer-offgrid-tiny-map](https://github.com/yuiseki/m5-cardputer-offgrid-tiny-map) (Apache-2.0), see its HACKSTER.md for the streaming design
- [OrcMaps](https://github.com/hardcoreerik/OrcMaps): `docs/FORMAT_DECISION.md`, `docs/PERFORMANCE.md`
- [mapnavigation](https://github.com/chaitanyaasati/mapnavigation): `maps/raster.cpp`, `tools/tile_format.md`
- [IceNav-v3](https://github.com/jgauchia/IceNav-v3) and [Tile-Generator](https://github.com/jgauchia/Tile-Generator)
- [OSMF vector tile usage policy](https://operations.osmfoundation.org/policies/vector/)

## Status (29 Oct 2026, PoC release candidate 0.0.1)

M0-M3 are done and tested on the T-Deck: keys, touch (drag, pinch, double tap, long press, overlay buttons, menus,
download screen) and the trackball in keyboard mode. Decisions as they are now, beyond the list above:
- Prepared tiles use 2048 units per tile, format version 2 (1024 looked stepped at z17-z19, 4096 was 35 % larger).
- Network tasks run on core 1 with their stacks in PSRAM; display strips are internal DMA memory; one tile conversion
  at a time; 28 cached tiles. These came from the memory measurements during downloads.
- An approximate (IP) position shows a ring with a "~25 km" label instead of an accuracy circle; GPS is unchanged.
- The map opens at ESA ESTEC the first time, then where it was left (view.txt).
- Developer options only in settings.txt: dev_mono (monochrome palette on any display), dev_probe (stall probes,
  memory log, HTTPS timing).

Moved to M4: 1-bit monochrome style, idle region updates, automatic region names (requested after the touch test),
trackball mouse mode polish, the 30-minute soak test.
