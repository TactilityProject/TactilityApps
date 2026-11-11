# Tactility Maps

Offline vector maps for [Tactility](https://github.com/TactilityProject/Tactility) devices. The device
downloads an area from [OpenFreeMap](https://openfreemap.org/) by itself and draws it fullscreen, with
anti-aliased roads, water, parks and buildings, from tiles on the SD card.

Version 0.2.0, tested on a LilyGO T-Deck (ESP32-S3, 8 MB PSRAM). Changes are listed in
[CHANGELOG.md](CHANGELOG.md).

## Install

Needs Tactility 0.8.0 or newer on an ESP32-S3 board with 8 MB PSRAM, a FAT-formatted SD card, and Wi-Fi
for downloading maps. Tested on Tactility 0.8.0-dev (main at 0e276da2) on a LilyGO T-Deck.

1. Install "Maps" from Tactility's App Hub. Or copy the `.app` file to the SD card, open it in the Files app and
   confirm "Install".
2. Start "Maps" from the app list. The first time it opens at ESA ESTEC in Noordwijk with no map data yet: move
   to your area, open the menu (M or ☰) and choose Download map.

## How it works

- Vector tiles (OpenMapTiles schema) are stored one file per tile in `maps/<region>/<z>/<x>/<y>.pbf` on the SD card.
- The map stops LVGL and draws with Tactility's `pixel_buffer` API, like the built-in Terminal app.
- Tiles are inflated and decoded as a stream, never held whole in memory, and drawn with an exact-area
  anti-aliasing rasteriser. Two render tasks (one per core) fill a PSRAM cache. Rendered tiles can also be
  cached on the SD card (a setting, off by default).
- Zoom levels above 14 are drawn from the z14 vectors, so they stay sharp.
- Each downloaded tile also gets a prepared copy next to it (`<y>.prt`): only what the style draws, with
  quantised coordinates, grouped by draw order. It is about a fifth of the original's size and renders faster.
  The original `.pbf` is never changed; it keeps names and house numbers for later features.
- One task does all SD card reads and writes, tiles on screen first. Tiles around the screen are only
  prepared after a second without input, and any input cancels them.
- The position comes only from Tactility's location service. The app never changes location settings.
- Menus, lists and dialogs are drawn by the app as well, LVGL stays stopped while the app runs.

## Using the app

**Map screen.** The map fills the screen. Always visible: your position (●, with an accuracy
circle), the scale bar, and the © mark.

| To… | Keyboard | Trackball | Touch |
| --- | --- | --- | --- |
| Move the map | W A S D | Roll | Drag |
| Zoom in / out | I / O | — | Pinch, double tap to zoom in, or the + / − buttons |
| Show / hide controls | Space | Click on empty map | Tap the map |
| Centre on my position and follow | G | — | ⌖ button |
| Stop following | Move the map | Roll | Drag |
| Turn the map | Q / E | — | Two fingers |
| Back to north | N | — | Compass button |
| Open a row's actions | Enter | Click | Tap the row |
| Add a waypoint | P | — | Hold ⌖ (your position) or hold the map (that point) |
| A place or house number | Show the controls, then Enter (nearest the crosshair; Enter again: next) | Click it | Tap it |
| Menu | M | Long click | ☰ button |
| Download an area | Shift + D | — | Menu → Download map |
| Leave the download frame | Esc | — | The round < in its top left corner |
| Type a name (waypoint, rename) | Keyboard | — | On-screen keyboard |
| Credits | — | Click © | Tap ©, tap again for About and licences |
| Leave the app | Esc or Backspace | Menu → Exit | Menu → Exit |

**Touch only.** Every function works with only a touch screen. On boards without a keyboard an
on-screen keyboard opens with every text field (new waypoint names, Rename): letters with Shift (twice
for caps lock), a 123 page with digits and signs, an ÁÉ page with the accented letters common in names,
Del, Space and OK; the field moves up above it. Lists and menus scroll by dragging and go back with a
swipe to the right or their <; the download frame has its own < to leave without downloading.

**Controls.** Hidden by default. When shown: ☰ menu top left, + and − in the middle of the right edge, ⌖
position bottom right above the scale bar. With a keyboard each button shows its key. ⌖ is filled while
following, an outline when not, and crossed out when there is no position. With the trackball in mouse
mode the pointer shows only while the controls or the menu are open, the button under it is highlighted,
and it hides after 3 s without movement (a click brings it back). The pointer works on every screen and
speeds up with movement; Settings → Pointer speed (Slow / Normal / Fast). A click on the map shows or
hides the controls. On boards with buttons only (no keyboard, no touch),
the buttons move the focus through the controls and press the focused one.

**Zoom.** + / −, I / O and double tap zoom smoothly (about a quarter of a second, around the screen centre,
or the tapped point for double tap), then the sharp map appears. Pinch scales live as before. This is off
on monochrome and e-paper displays, or with Settings → Smooth zoom off (the setting was called Smooth
pinch).

**Rotation.** Q / E turn the map by 15°, N or the compass (top right, only shown on a turned map) turns
it back to north. Two fingers turn it once twisted past 45° (15° when the map is already turned), so
pinching does not turn it by accident.
⌖ / G cycles: follow north up → follow with the direction of travel up (from the GPS course while moving
faster than about 1 m/s, smoothed, held when slower) → not following. Keys and dragging move a turned map
in screen directions; labels stay upright; the download frame always works north up. On monochrome and
e-paper displays turning is not animated.

**Labels.** Place, street, river, water and peak names are placed for each view so they never overlap;
street names follow the street and stay upright, with a light halo. Settings → Labels (Off / Few / Normal)
and Settings → Latin names (the Latin-script name where the map data has one). The app ships a subset of
Adwaita Sans with Latin, Greek, Cyrillic and Vietnamese letters, so names like Štefánikova, Łódź,
İstanbul, Москва or Hà Nội show fully; the device makes the fonts from it once (about 7 s in the
background on first start) and keeps them on the card, made again once when a new app version brings a
new font. The font holds the characters map names use (scanned from real map data of many countries) plus
whole Latin, Greek and Cyrillic alphabets, 39 KB. Map data downloaded with an older version is upgraded in
place in idle time, the tiles on screen first; no download is needed.

**Places and house numbers.** Icons for cafés, shops, stops, stations, pharmacies, hotels, museums and
about 120 other kinds of places, coloured by kind (food orange, shops purple, transport blue, health red,
…), from z15 (stations, museums and the most important places from z14, benches and bins from z16), with
their names next to them from z17 when there is room. House numbers in small grey text from z17. Both
follow Settings → Labels (one zoom level later with Few) and never cover place or street names. Tap or
click one for a sheet with its name, kind, coordinates, distance and direction, "Save as waypoint" (the
name filled in) and Close. With keys: show the controls (a small crosshair marks the middle), then Enter
opens the place nearest the crosshair and each next Enter the next nearest; Esc closes the sheet. Enter
on the map without the controls still shows them. Icons: Maki by Mapbox (CC0).

**Background work.** A small amber dot in the top-left corner shows while the app works in the
background: preparing or upgrading tiles, making label fonts, updating maps, importing GPX files, moving
browsed tiles into a download. It pulses on colour screens and is solid on monochrome and e-paper ones.
It hides while the progress label or the controls show. Tap or click it for a card listing what runs.
Not to be confused with the red ● of track recording.

**Navigation.** Routes are found on the device from the maps you downloaded: no internet and no extra
download. A place, a waypoint, a house number or a point you hold on the map opens a sheet with **Go**
(Navigate here; key N or G on a place or waypoint). The route starts at your position when there is a GPS fix.
Without one the app asks where it starts: **Choose on the map** (tap the start, or move the map and press Enter
for the crosshair; Esc cancels), **Screen centre**, or **My position** when Tactility has an approximate position
(Wi-Fi or IP based; its accuracy is shown, e.g. ±25 km). It is searched in the background; the map then shows the whole route,
a bar with the three modes at the top and a card with its length, time and mode, and **Start** and **Close** (keys
S, Esc).

- **Modes:** Walk, Bike and Car, as icons at the top. Tap one (or press W, B, C, or 1, 2, 3; P goes to the next) to
  search the same route for that mode, also while a search still runs. A new route starts with the mode set in
  Settings → Route mode (Walk unless changed). When a search fails the card says why and stays, so another mode can
  be tried. Walk uses every road and path and ignores one-way streets; Bike leaves out motorways and
  trunk roads and keeps to one-way streets; Car leaves out paths, footways, steps and tracks and keeps to one-way
  streets. Time estimates: walk 4.5 km/h, bike 15 km/h, car by road type (110 km/h on motorways down to 20 km/h
  on service roads).
- **Car routes follow road types only**, without turn restrictions or access rules (the map tiles have
  neither): the card says so. Check signs.
- **Start** follows the route: the map turns with your direction (heading up), your position sits low on the screen
  so you see further ahead, and a bar at the top shows the next turn with an arrow and the street, e.g. "In 120 m
  turn left onto Štefánikova", and below it the distance and time left. The X at the right end of the bar (or Esc)
  stops navigating; the menu button is below the bar. The route ahead is blue, the part you passed grey. More than 40 m off the route for 10 seconds:
  "Off route — recalculating", and a new route from where you are (at most every 20 s). Within about 25 m of the
  destination: "You have arrived"; following stops and the line stays.
- **Start and destination** snap to the nearest road the mode may use within 200 m (500 m for a car: the last bit is
  on foot), leaving out small pieces of road that lead nowhere for that mode, such as a service road inside a
  pedestrian zone for a car.
- **The route stays** until Menu → Clear route, also after leaving the app. Menu → Show route shows it and its
  card again; Menu → Stop navigation stops following.
- **Limits:** a search may use about 2.5 MB of memory (less when the map holds more). When the memory runs out it
  tries once more with a search that heads straight for the destination, which may give a slightly longer route;
  only if that fails too, very long routes (across a big city, corner to corner) stop with "Route too long for this
  device; try a shorter one". Routes are found only where the tiles were
  downloaded at the highest detail (z14) and prepared: right after a download the app first prepares the new
  tiles in idle time (the amber dot). Without a road within 200 m of the start or the destination, or without
  a connection between them in the downloaded map, the app says so.

**Menu:** Download map, Downloaded maps, Waypoints, Add waypoint, Record track, GPX files, Location, Settings, About and
licences, Exit. W/S, trackball or finger to move; Enter, click or tap to choose; Esc, Backspace or tap
outside to close.

**Lists and actions.** Enter or a tap on a row opens its actions; lists have a back arrow and a check
mark in their header; swipe right or the back arrow goes back, and dragging a list scrolls it. Lists open
without a highlight until a key, the trackball or the encoder moves.

**Downloading:** open Download map. It asks "Choose area" or "Choose region". "Choose area" keeps the frame
in the middle; move and zoom the map under it. The bar shows tiles, size and max zoom, and updates live.
Enter, click or Download starts it; Esc cancels. It downloads in the background and resumes after a
restart; areas share tiles. Tiles you already looked at while browsing are not fetched again: they move
from "browsed" into the new region (the bar says e.g. "1240 tiles, 310 already on the card"); a browsed
tile from an older map build is downloaded fresh.

**Choose region** lists continents, countries and regions, from Geofabrik's region list shipped with the
app. Enter or tap opens a region's parts; the first row "All of …" downloads the whole of it. The app
picks the highest zoom level at which the whole region fits OpenFreeMap's limit of 3,000 tiles or 50 MB,
and shows the tile count, size and zoom level before you confirm. Only the tiles inside the region's
outline are fetched.

**Downloaded maps:** list with name, size, date and status (complete, downloading, outdated). Select with W/S,
trackball or tap; delete with Del then confirm, or long press then Delete; R renames (needs a keyboard);
Esc goes back. Enter or tap on an outdated region offers "Update now". Deleting keeps tiles another
region still uses.

**Region names:** a new download gets a name: a chosen region its own name; a framed area the name of the
nearest town in its map data once it has downloaded (before that, the smallest region of the list it lies
in). Downloaded maps shows the names. The folder on the card keeps a short name (8.3).

**Browsing:** with Wi-Fi, tiles on screen that have no detail on the card yet are downloaded while you
look at them (a small "Downloading map" label shows) and kept in the region "browsed", so they work
offline later. Without Wi-Fi a short message says that detail is missing here. Only the tiles on screen
are fetched, one at a time.

**Settings:** SD card cache on or off, prepared tiles deflated or raw, smooth zoom on or off, download
while browsing on or off. Defaults: SD card cache off (switched off, its files are deleted in idle time),
deflated, on, on. The map opens at ESA ESTEC, Noordwijk, the first time, then where you left it.

**Updates:** OpenFreeMap rebuilds its map about weekly. A region downloaded from an older build shows
"outdated" in Downloaded maps. After about 30 s without input the app updates outdated regions quietly, one tile
at a time, the tiles you look at first; a small "Updating 42%" label shows meanwhile. Any key, touch or
trackball movement pauses it, and it continues later, also after a restart. Only on Wi-Fi, and only while
charging or above 40 % battery (boards that report no battery: Wi-Fi alone decides). Enter (or tap) on an
outdated region in Downloaded maps offers "Update now". The region "browsed" (tiles fetched while browsing) is
not updated.

**Position:** comes from Tactility's location service (GPS or approximate). Menu → Location switches
Tactility's location service on or off, with a message. If it's off, the ⌖ button is crossed out and
pressing it says how to enable it in Tactility's settings.

**Waypoints:** P, or holding ⌖, saves a waypoint at your position; Menu → Add waypoint saves one at the
map centre; on touch screens holding the map saves one where you hold. A sheet asks for the name of a
new waypoint; typing replaces the suggested name. Waypoints are named WP 001, WP 002, …; they show as
pins at every zoom level, all the same size. Tap a pin for its card: name, coordinates and distance and
direction from you. Esc closes the card. Menu → Waypoints lists them by distance: Enter or tap goes
there, R renames (keyboard), Del or hold deletes after asking. They are kept in `waypoints.gpx` (GPX 1.1)
in the app's folder on the card.

**Track recording:** Menu → Record track → Start recording. A red ● with the time shows top right (with
the controls shown also the distance). The same menu entry pauses, resumes and stops. Points are taken
from the position when you moved more than 5 m (or more than the GPS accuracy), or every 30 s while
moving; approximate positions (from the network) are skipped. The track is drawn on the map as a red
line. It is written to `tracks/YYYYMMDD-HHMM.gpx` every 30 s, so a crash loses at most 30 s, and a file
left open is closed the next time the app starts. Recording only runs while the app is open (background
recording would need Tactility support; that is a question for later). "Recorded tracks" in the same menu
lists the files with their folder, so they can be copied off the card.

**GPX files:** copy `.gpx` files into the folder `gpx` at the top of the SD card (`/sdcard/gpx`). Menu →
GPX files lists them; Enter or tap shows or hides one. Tracks are drawn blue, routes purple, waypoints
as small dots. A file is read once and a compact copy (`.gpc`) is kept next to it, so it shows quickly
next time. Files that are not GPX show an error and are skipped. Large tracks were tested on the PC (50,000 points
read in about 40 ms there), not yet on the device.

## OpenFreeMap's rules

OpenFreeMap is free and needs no key. Its [terms of service](https://openfreemap.org/tos/) for offline downloads
are enforced in the app:
at most 3,000 tiles and 50 MB per download (the highest zoom level that fits is chosen, the download also
stops at 50 MB of real data), at most 4 parallel requests (the app uses 2), and a User-Agent that names the
app. The tile URL is read from OpenFreeMap's TileJSON. openstreetmap.org tile servers are never used.

## Known limits

- One download is at most 3,000 tiles or 50 MB (OpenFreeMap's terms). In dense cities that is only a few hundred
  z14 tiles, for example the centre of Budapest. Larger areas need several separate downloads, which you start
  yourself.
- Labels show no characters outside Latin, Greek, Cyrillic and Vietnamese. Names in other scripts (for
  example Japanese, Chinese or Arabic) show their Latin-script name instead, where the map data has one.
- Heading-up needs movement (no magnetometer: the course comes from GPS).
- Not yet tested outdoors with GPS.
- Touch, keyboard and trackball were tested on the LilyGO T-Deck only. Other boards, boards without PSRAM and
  monochrome or e-paper displays are untested.
- 1-bit displays get their own black-and-white style, so far checked only in renders on the PC, not on a 1-bit device.
- The trackball in mouse mode is only lightly tested.
- Recording stops when the app closes (background recording would need Tactility support).

## Credits

- Map data: [OpenStreetMap](https://www.openstreetmap.org/copyright) contributors, available under the Open
  Database License (ODbL).
- Vector tile schema: [OpenMapTiles](https://openmaptiles.org/).
- Tiles served free of charge by [OpenFreeMap](https://openfreemap.org/), see its
  [terms](https://openfreemap.org/tos/).
- Region outlines © OpenStreetMap contributors, from Geofabrik GmbH (download.geofabrik.de), ODbL 1.0.
- Built for [Tactility](https://github.com/TactilityProject/Tactility).
- Label font: Adwaita Sans, under the SIL Open Font License 1.1; see [THIRD_PARTY.md](THIRD_PARTY.md).
- Place icons: [Maki](https://github.com/mapbox/maki) by Mapbox, CC0 1.0.

## Attribution

The full "© OpenMapTiles © OpenStreetMap contributors" shows for 5 seconds when the map opens and then
collapses to a small "©" mark in the bottom left corner. Tapping or clicking it shows the text again for 5
seconds, tapping the text opens "About and licences", which is also in the menu. This follows the OSMF
attribution guidelines for small screens.

## Building

Needs ESP-IDF 6.1 and the Tactility SDK matching the firmware on the device.

```
python <Tactility>/tt.py app build
python <Tactility>/tt.py app install --host <device ip>
python <Tactility>/tt.py app run --host <device ip>
```

The PC build (`host/`, CMake) renders tiles to PNG and runs the unit tests:

```
cmake -S host -B host/build
cmake --build host/build --config Release
host/build/Release/render_tile <folder with maps/> 47.4979 19.0402 14 640 480 out.png
host/build/Release/tests
host/build/Release/prepare_tiles <folder with maps/> [--write] [--deflate] [--zoom 16]
host/build/Release/ui_preview <output folder> [--mono]
```

`prepare_tiles` converts the tiles of a folder into prepared tiles and compares size, render time and pixels
with the originals (`--write` stores the `.prt` files, which `render_tile --prepared` then draws from).
`ui_preview` draws the app's screens over a fixture tile to PNG, with blocks in place of text.

`host/fetch_tiles.py` downloads a test area on the PC (same OpenFreeMap rules), `host/push_region.py`
copies it to a device through the development server.

## Licence

GPL-3.0-or-later, the same as Tactility; see [LICENSE](LICENSE). Third-party code and data are listed in [THIRD_PARTY.md](THIRD_PARTY.md). No code was copied from
the projects listed as prior art in the plan.
