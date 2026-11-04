# Changelog

## 0.2.0

Labels, map rotation and heading-up, plus menu, list, trackball and download fixes.

**Labels**
- Place, street, river, water and peak names, placed for each view so they never overlap; street names
  follow the street and stay upright; a light halo.
- Settings → Labels (Off / Few / Normal), Settings → Latin names (the Latin-script name where the map data
  has one). Names in scripts the label font has no letters for (Japanese, Chinese, Arabic and others) always
  show their Latin-script name.
- Label fonts: the app ships a subset of Adwaita Sans with Latin (Extended-A and -B), Greek, Cyrillic and
  Vietnamese letters, so names like Štefánikova, Łódź, İstanbul, Москва or Hà Nội show fully; the device
  makes the fonts from it once (about 7 s in the background on first start) and keeps them on the card.
- Map data downloaded with 0.1.0 is upgraded in place in idle time, the tiles on screen first; no download
  is needed.

**Rotation**
- Q / E turn the map by 15°, N or the compass (top right, only shown on a turned map) turns it back to
  north; two fingers turn it once twisted past 45° (15° when already turned), so pinching does not turn it
  by accident.
- ⌖ / G now cycles: follow north up → follow with the direction of travel up (from the GPS course while
  moving faster than about 1 m/s, smoothed, held when slower) → not following.
- Keys and dragging move a turned map in screen directions; labels stay upright; the download frame always
  works north up.
- On monochrome and e-paper displays turning is not animated.

**Menu and lists**
- Menu → Location switches Tactility's location service on or off (with a message).
- Enter or a tap on a row opens its actions; lists have a back arrow and a check mark in their header,
  swipe right goes back; lists open without a highlight until a key, the trackball or the encoder moves.
- Menu names: Download map (Choose area, Choose region), Downloaded maps.

**Trackball and touch**
- The trackball works as a mouse pointer on every screen, with acceleration; Settings → Pointer speed
  (Slow / Normal / Fast).
- A click on the map shows or hides the controls; holding the button no longer opens the menu. Dragging a
  list scrolls it.

**Waypoints**
- A sheet to name a new waypoint (hold the map, hold ⌖, P, or Menu → Add waypoint); typing replaces the
  suggested name. All pins are the same size.

**Downloads**
- A progress pill with tiles and percentage, and a details card (tap the pill) with Pause, Resume and
  Cancel, speed and time left.
- Detail fetched while browsing: tiles no longer needed after you move on are dropped from the queue (the
  one downloading finishes).
- A download that fails (network or memory) tries again by itself after 30 s, up to 3 times; the message
  stays 10 s.
- A framed area is named after the nearest town's Latin-script name, so the lists can show it.
- Framing an area a region already covers does not download it again: an unfinished region continues, a
  finished one says so.

**Fixes**
- The map could stop responding for many seconds while big tiles loaded.
- Zooming out kept a blank screen until tiles loaded (the previous map now stays, shrunk).
- Browsing downloads continued after panning only after a zoom change.
- The progress pill stayed after browsing ended.
- A large low-zoom tile is read from the card once instead of once per zoomed-in tile.
- Downloads failed late in a session: the large low-zoom tiles (over 1 MB) needed one free block of memory
  that size. Tiles now download in 64 KB pieces, and the tile cache lends memory when even a piece does not fit.

## 0.1.0

Idle updates, named regions, waypoints, track recording and GPX files, plus smooth zoom.

**Zoom**
- + / −, I / O and double tap zoom smoothly (about a quarter of a second, around the screen centre or the
  tapped point), then the sharp map appears. Pinch scales live as before.
- Off on monochrome and e-paper displays, or with Settings → Smooth zoom off (renamed from Smooth pinch).

**Regions and downloads**
- Download area asks "This area" or "Choose region". Choose region lists continents, countries and regions
  from Geofabrik's list, shipped with the app. "All of …" downloads a whole region.
- The highest zoom level that fits OpenFreeMap's 3,000-tile / 50 MB limit is chosen automatically, with the
  tile count, size and zoom shown before you confirm. Only tiles inside the region's outline are fetched.
- Downloads get names: a chosen region its own name, a framed area the nearest town once downloaded. The
  Regions screen shows the names; R renames (keyboard). Folders keep 8.3 names.
- Outdated regions update quietly after about 30 s without input, one tile at a time, the tiles you look at
  first, with an "Updating 42%" label. Any input pauses it and it continues later, also after a restart.
  Only on Wi-Fi, and while charging or above 40 % battery.
- Enter or tap on an outdated region offers "Update now". The region "browsed" is not updated.
- HTTPS connections are kept open and shared, so a new download or browsing no longer pays a 2-3 s TLS
  handshake each time.

**Waypoints**
- P, holding ⌖, Menu → Add waypoint, or holding the map on touch screens saves a waypoint. Named WP 001,
  WP 002, …, shown as pins at every zoom level.
- Tap a pin for name, coordinates and distance and direction from you. Menu → Waypoints lists them by
  distance; Enter or tap goes there, R renames, Del or hold deletes after asking.
- Kept in `waypoints.gpx` (GPX 1.1) in the app's folder on the card.

**Tracks**
- Menu → Record track: start, pause, resume and stop. A red ● with the time shows top right, with the
  distance when the controls are shown.
- Points every 5 m (or more than the GPS accuracy) or every 30 s while moving; approximate positions are
  skipped. The track is drawn as a red line.
- Written to `tracks/YYYYMMDD-HHMM.gpx` every 30 s, so a crash loses at most 30 s. "Recorded tracks" lists
  the files with their folder.
- Recording only runs while the app is open.

**GPX files**
- Copy `.gpx` files into `/sdcard/gpx`. Menu → GPX files shows or hides them.
- Tracks blue, routes purple, waypoints as small dots. A compact `.gpc` copy is kept next to each file.
  A 50,000-point track was tested on the PC, not yet on the device.

**Trackball**
- In mouse mode the pointer shows only while the controls or the menu are open, the button under it is
  highlighted, and it hides after 3 s without movement.
- A click brings it back, and a click on the map while the controls are hidden shows them.

**Displays**
- A black-and-white style for 1-bit displays: black roads with white edges, water and parks as sparse
  patterns, outlined buildings, no grey. Checked in renders on the PC only.

**Requirements**
- Needs a board with 8 MB PSRAM for now.

## 0.0.1 (proof of concept)

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
