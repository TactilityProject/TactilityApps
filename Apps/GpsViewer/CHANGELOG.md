# Changelog

## 0.1.0

First release: a GNSS status viewer with four tabs, a sky plot, settings and NMEA recording.

**Screen**
- Tactility toolbar with close button, title, fix state and menu button. Tabs Position, Sky, Signal, Status.
- Flat layout with dividers, in the Tactility theme's colours. Night mode in reds on black.
- Position: coordinates, altitude, course; speed with maximum and average; sunrise and sunset (NOAA algorithm,
  offline); UTC and local time.
- Sky: polar plot, north up, rings at 0, 30 and 60 degrees elevation. Shape and colour per system, filled when
  used. Legend with used / in view per system.
- Signal: SNR bar per satellite, grouped by system, coloured by strength, outlined when not used. Scrolls
  sideways.
- Status: fix type, estimated accuracy (HDOP x 5 m), HDOP / VDOP / PDOP with a quality word, satellites used and
  in view, time to first fix, receiver model.
- A message with "Open settings" when there is no receiver or location is off.

**Data**
- Subscribes to Tactility's location service and reads the first started GPS receiver.
- Satellites are told apart by the NMEA talker ID when the firmware gives it. Without it, by ID ranges only.

**Recording**
- Saves the receiver's RMC, GGA, GSA and GSV sentences to `logs/gps-<date>-<time>.nmea`, written every 2 seconds.

**Settings**
- Units (metric, imperial, nautical), coordinate format (degrees, degrees and minutes, degrees minutes
  seconds), night mode. Kept in `settings.txt`.

**Input**
- Keyboard, trackball and touch. Keys 1-4 jump to a tab, `n`, `r`, `s`, `l` and `m` act on night mode,
  speed reset, settings, recording and the menu.
