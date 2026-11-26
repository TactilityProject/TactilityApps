# GPIO

Shows the live state of all GPIO pins of the device. Use it to check wiring and to see which pins are high or low.

- The row at the top has the **Grid** and **Timeline** tabs, with freeze and reset on the right.
- Every pin is a numbered tile. A filled tile is high.
- A coloured border marks a pin that changed in the last two seconds.
- Tap a tile (or move with the arrow keys) to see its level, the number of changes and when it last changed.
- **Pause** (or Space) freezes the screen. Changes during a freeze aren't counted.
- **Reset** clears the change marks, counters and traces.
- Long press a tile to pin it to the timeline, or to unpin it. Pinned tiles have a bar at the bottom.

## Timeline

- **+ GPIO** adds a pin, and tapping a lane's number removes it.
- **Every ...** sets how often the pins are read, from 20 ms to 1 s, for both tabs. Changing it empties the traces.
- The pinned pins and the interval are remembered.

Pulses shorter than the interval can be missed, and short intervals are uneven when the device is busy. Pins are only read, never changed.
