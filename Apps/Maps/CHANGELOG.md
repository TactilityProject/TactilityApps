# Changelog

## 0.0.1 (proof of concept, not released yet)

The first public version: an offline vector map for Tactility, tested on the LilyGO T-Deck.

**Map**
- Fullscreen map drawn by the app with LVGL stopped. LVGL comes back on every exit.
- OpenFreeMap vector tiles (OpenMapTiles schema) from the SD card: water, land cover and use, parks, buildings,
  waterways, roads with casings, railways, boundaries and airports, anti-aliased.
- Zoom 0-19. Levels above 14 are drawn from the z14 vectors, so they stay sharp.
- Pan and zoom by keys (W A S D, I / O), trackball, drag, pinch, double tap and the + / − buttons.
- Your position from Tactility's location service, with an accuracy circle, a direction arrow while moving and
  follow mode (G or ⌖). The app never changes location settings.
- Scale bar, and the attribution: shown in full for 5 s, then a © mark that opens it again and leads to
  About and licences.
- Hidden controls (☰, + / −, ⌖) that show with a tap, Space or a click, with key hints on keyboards and a
  pointer for the trackball in mouse mode.
- Opens at ESA ESTEC the first time, then where you left it.

**Downloads**
- Download area: a fixed frame in the middle, the map moves and zooms under it. A live tile count, size estimate
  and the highest zoom that fits OpenFreeMap's 3,000-tile / 50 MB limit.
- The downloader uses OpenFreeMap's TileJSON, 2 keep-alive HTTPS connections and its own User-Agent. Tiles are
  written as `.tmp` and renamed. Downloads resume after a restart, areas share tiles, and the download stops at
  50 MB of real data.
- Download while browsing: with Wi-Fi, tiles on screen that have no detail yet are fetched one at a time and kept
  in the region "browsed".
- Regions screen: size, date and status, with delete (tiles other regions use are kept).

**Speed**
- Prepared tiles (`.prt`) next to each `.pbf`: only what the style draws, quantised to 2048 units per tile,
  deflated. About a fifth of the size, half the render time. Converted in idle time.
- One I/O task for all SD card access, tiles on screen first. Two render lanes, a PSRAM cache of 32 rendered
  tiles, and prefetch only after a second without input.
- Cold first z14 screen in dense central Budapest: 1.8-2.0 s on the T-Deck (5.6 s in the first device build).

**Known limits**
- At most 3,000 tiles or 50 MB per download. No labels yet. Touch tested on the T-Deck only. No automatic
  region updates yet.
