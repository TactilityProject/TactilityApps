# Changelog

## 0.5.0

Offline search of places, streets, addresses and kinds of places in the downloaded maps.

- Menu → Search, / or F: results while typing from 3 letters, nearest first, with type, distance and direction;
  addresses as "street number" in either order; diacritics and Greek/Cyrillic in Latin letters don't matter; a
  shorter beginning is tried when nothing matches. Nearby by category, the last 10 chosen results.
- A chosen result is shown on the map with a mark and its sheet with Go and Save as waypoint; streets fit the screen.
- House numbers get the nearest street within 50 m, up to 100 m marked as estimated; village addresses in regions
  where most numbers are plain.
- The search index per region (`maps/<region>/search.idx`, about 1.5 MB for Bratislava) is made on the device in
  idle time, the region on screen first, and again after the region's tiles are updated; big regions get one index
  per block of 15 x 15 tiles. Bratislava: about 5 minutes on a T-Deck, the same file the PC tool makes. On the T-Deck
  a keystroke takes 1-150 ms (Bratislava).
- House numbers also take named paths and pedestrian ways as their street (88 % right in Bratislava against OSM
  addresses, 87 % without).
- Results show the town or village they are in ("Street · Holice"), and the city district inside a city or town
  ("Address · Ružinov, Bratislava"), so the same street name in two places can be told apart. Estimated from the
  places in the map data; works with indexes already on the card.

**Fixes**
- While a region downloads, the tiles on screen get their POIs and house numbers after a second without input,
  instead of after the whole download (they were not prepared while any download ran). The tile cache makes room
  for them, and a tile on screen that the download moves into the new region stays first in line.
- Letters the UI font lacks (such as š in "Spišská Nová Ves") are drawn as their base letter instead of being left
  out.

## 0.4.1

Leaving the route while navigating is noticed sooner when the GPS is good, and not by mistake when it is poor.

- The off-route limit follows the GPS precision: the median accuracy of the last 10 s of fixes (a single bad fix
  doesn't count) gives a limit of 1.5 x accuracy + 5 m, between 15 m and 60 m (was 40 m, or accuracy + 25 m).
- Three positions in a row beyond the limit are off route (was 10 s). With poor GPS a few more are needed (25 m
  accuracy: 8), because GPS noise of that size alone crosses the limit now and then on a long walk.
- Going into another street is noticed earlier: moving faster than 1 m/s more than 60 degrees away from the route's
  direction, or being clearly nearer to a side street than to the route (with good GPS), counts from two thirds of
  the limit.
- The first new route is searched at once, later ones at most every 10 s (was 20 s); a new route settles for two
  positions before it is checked.
- Worse than 50 m accuracy: "Weak GPS" on the navigation bar, the route is followed without recalculating.
- Fixes without an accuracy count as 30 m (Tactility gives HDOP x 5 m when the GPS reports HDOP).
- For testing: the "replay" debug command can add GPS noise and a set accuracy, "offroute" changes the rule's
  values on the device, and the serial log has one line per position while navigating.

## 0.4.0

Offline navigation: routes for walking, cycling and driving, found on the device from the downloaded maps, with a
preview and turn-by-turn following. Prepared tiles are split into smaller files, so maps open and label faster.

**Navigation**
- "Go" (Navigate here) on the sheet of a place, a house number, a waypoint and a point held on the map. The route
  starts at your GPS position; without a fix the app asks: a point you tap (or the crosshair), the screen centre, or
  the approximate Wi-Fi/IP position with its accuracy.
- Modes Walk, Bike and Car as icons at the top of the route preview: one tap (or W, B, C) searches the same route
  for another mode, also during a search. New routes start with Settings → Route mode. Car routes follow road
  types only, without turn restrictions or access rules, and the card says so.
- The map shows the whole route as a blue line with a white casing (a thick dark line on monochrome displays) and a
  card with length, time and mode, and Start and Close. A failed search keeps the card with the reason.
- Start: heading-up following with a bar for the next turn and the street ("In 120 m turn left onto ..."), the
  distance and time left; passed parts turn grey. Off the route for 10 s: a new route from where you are. Within
  about 25 m: "You have arrived".
- The route is kept until Menu → Clear route, also across restarts. Menu → Show route and Stop navigation.
- The road graph is made per z14 tile in idle time from the tiles on the card (a `.prg` next to each tile).
- When the search runs out of memory it tries once more heading straight for the destination (the route may be a
  little longer); only then do long routes stop with a message. On the T-Deck 47 of 50 Bratislava test routes on foot
  and 48 of 50 by car are found (was 42 and 38).
- The map no longer blinks while a route is searched: tiles around the screen are not drawn meanwhile.
- Start and destination no longer snap to small pieces of road that lead nowhere for the mode (a car on a service road
  in a pedestrian zone or on a one-way piece, a short path off the network): such searches read the whole city and
  ended "too long" or "no route". A car may stop up to 500 m from the destination.
- Key hints (the letters on the buttons, "Enter: change" in lists) only on boards with a keyboard that types:
  found with Tactility's device discovery, so buttons (Wio Tracker) and a trackball alone no longer count.
- While navigating your position sits low on the screen; the bar has an X to stop (Esc too), and the menu button is
  below the bar.

**Faster maps**
- Prepared tiles are three files: geometry, labels and the routing graph. Drawing reads only the geometry (a dense
  city tile 127 KB → 69 KB); labels are read when they are placed, house numbers only from z17, and only the parts
  of a tile near the screen. Older tiles are split in idle time ("Splitting tiles"); they keep working meanwhile.
- Label placement with POIs takes about half as long as before.

**For developers**
- Debug script commands `routebench`, `navigate`, `waitroute` and `replay` (a GPX track as positions, to test
  following indoors). Host tools `graph_tiles` and `route_tiles`, and `host/routing/check_prg.py`, which compares the
  device graphs with the PC study.

## 0.3.0

Points of interest and house numbers, a background work indicator, smoother turned maps, a smaller label font, and
browsed tiles that move into a download instead of being fetched again. Needs Tactility main from November 2026
(app data in `appdata/`).

**Points of interest and house numbers**
- Icons for cafés, shops, stops, stations, pharmacies, hotels, museums and about 120 other kinds of places, from
  z15 (the most important from z14), with names next to them from z17. House numbers from z17. They follow the
  Labels setting and never cover place or street names.
- Tap or click a place or number for a sheet with its name, kind, coordinates, distance and bearing, and
  "Save as waypoint". With keys: show the controls, then Enter picks the place nearest the crosshair in the
  middle, and each next Enter the next nearest.
- Map data from 0.2.0 is upgraded in place in idle time; no download is needed.

**Background work**
- A small amber dot in the top-left corner while the app prepares tiles, makes fonts, updates maps, imports GPX
  files or moves browsed tiles. Tap or click it for a list of what runs.

**Map**
- A turned map is drawn smoothly (bilinear) once it stops moving; while it moves it stays fast.
- Tiles fetched while browsing move into a region you download over them instead of being downloaded again. The
  download screen says how many are already on the card.

**Touch only**
- Every function works with only a touch screen. On boards without a keyboard an on-screen keyboard opens with
  every text field (new waypoint names, Rename), with Shift, digits and signs, and accented letters.
- The download frame has a round < to leave it without downloading.
- The card for tiles fetched while browsing shows only Close: they can't be paused or cancelled.

**Downloads and background work**
- Tiles of a download are prepared later, once the map was left alone for 10 s, not right after the download. Tiles
  drawn on screen are still prepared at once.
- When memory is short, preparing waits and tries again instead of giving up on the tile until the map reopens. The
  tile cache and the render buffers make room for it, and get it back afterwards.
- Tiles a download moved out of the browsed tiles are no longer reported as failed preparations.
- Downloads use one connection instead of two: as fast on the T-Deck, without the TLS errors from running out of
  internal RAM and the reconnects they caused, and with more internal RAM left for the rest of the app.
- Tiles fetched for the screen while browsing are written to the card at once, not only once the map is left alone.
- Tiles on screen no longer stay drawn from a coarser zoom level when PSRAM has no block large enough for their
  data (low-zoom tiles are 300-450 KB before they are prepared). The tile cache makes room, or the tile is read
  from the card a piece at a time; if neither works it is tried again a few seconds later.
- A tile that was drawn around the screen and then came into view with coarser data is fetched in full detail.
- The background-work dot is a solid dot without animation, and no longer flickers through the dimming of an
  opened menu.

**Label font**
- The app's label font holds just the characters map names use, plus whole Latin, Greek and Cyrillic alphabets:
  39 KB instead of 80 KB. The sizes on the card are made again once after the update.

**Tactility**
- App data moved to `/sdcard/tactility/appdata/tactility.maps` with the new Tactility; the app moves anything
  left in the old folder on first start.

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
