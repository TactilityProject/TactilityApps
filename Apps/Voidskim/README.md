# Voidskim

An arcade 3D racing game for Tactility. Hovering craft race around tracks suspended over a void. Ramps launch you over gaps, and edges without barriers let you fall off. Falling puts you back on the track a few seconds later.

Rendering uses [Jet](https://github.com/cubecoders/jet) (MIT), a fixed-function software rasteriser, included as a git submodule in the repository's `Libraries/Jet`.

## Playing

From the menu, pick a track, a craft, the number of opponents (0-5) and the difficulty, then start the race. You start at the back of the grid.

With no opponents it's a time trial: the HUD shows the current lap time and the best lap to beat, each lap shows how far ahead or behind you are, and the results list every lap. The best race time and best lap of each track are kept in `records.txt` in the app's user data directory.

| Action | Keyboard (default keys) | Trackball | Touch |
| --- | --- | --- | --- |
| Steer | Left/Right, A/D | roll left/right | arrow buttons |
| Accelerate | Up, W | roll up | GO |
| Brake / reverse | Down, S | roll down | BRK |
| Boost | Space, B | click | BST |
| Pause | Escape, P | | pause button (top) |
| Menus | arrows, Enter, Escape/Backspace, or the race keys | roll, click | tap a row (left half goes back, right half forward) |

**Settings** (in the main menu, and in the pause menu during a race) has the audio (music and effects, effects only, or off), the **3D quality** (low, the default, renders the world at 50% of the width and height and scales it up, medium at 75%, high at the full resolution: lower is faster on big screens, while the HUD and menus stay sharp), a frame rate counter, the **touch layout** (steering buttons on the left and the others on the right, or the other way round) and **Controls**, where you change the keys: pick a control and a slot, then press the new key (each control has two). A key belongs to one control at a time, so taking a key moves it. **Defaults** restores the keys above. Settings are saved in `settings.txt` in the app's user data directory.

Touch controls are shown on devices with a touch screen. They hide when you use a keyboard and come back when you touch the screen.

Boost drains a tank (the bar under the lap timer) and recharges when you're not using it. The craft with the biggest tank lasts longest.

## Tracks

Tracks are plain text files in `assets/levels/*.trk`. Extra `.trk` files in the app's user data folder under `levels/` are added to the menu too. Everything is in metres and degrees:

```
# a comment: '#' at the start of a line, or followed by a space
name My Track
laps 3
width 16           # track width; changes ease in over the next segment
walls both         # barriers: both | left | right | none (no barrier = you can fall off)
walls both easy    # ...followed by difficulties (easy normal hard): only on those
height 9           # road height; eases in over the next segment (at most 0.3 m per metre)
tunnel on          # covered road (walls and a roof) until "tunnel off"
straight 120
curve 90 60        # angle (positive turns right) and radius
ramp 22 3          # kicker ramp: length and height. You jump off its end
gap 16             # no road: jump it or fall
colour sky #0B3D91 # sky, horizon, below, road, road2, edge, edge2, wall, ramp, structure, scenery
```

The track must end where it started, facing the same way, and back at height 0. Small position mismatches are spread out over the lap. Larger ones are reported as an error, and the track is left out of the menu.

A track may cross itself, but only at different levels: one part has to pass at least 7 m above the other, on a bridge (the lower part usually runs through a tunnel). Neon Knot, a trefoil knot, crosses itself three times this way.

A track's music is the `.bleep` file next to it with the same name (see Sound).

## Craft

Craft are defined in `assets/vehicles.txt`, one block per craft:

```
vehicle Arrow      # name
shape dart         # dart | wing | hauler
colour #E63946     # body
accent #F1FAEE     # trim
speed 68           # top speed, m/s
accel 21           # acceleration, m/s^2
brake 45
turn 1.8           # turn rate at full lock, radians/s
grip 3.5           # how quickly sideways drift dies out
boost 20           # extra top speed while boosting
boosttime 2.5      # seconds in a full tank
recharge 0.2       # tank fraction refilled per second
mass 1.0           # weight in collisions
air 0.3            # steering left while airborne (0-1)
```

The three stock craft play differently:
- **Arrow:** the fastest at the top end but the slowest to get there, with the smallest boost. Twitchy: it turns slowest.
- **Swift:** the all-rounder: middle of the pack in speed, acceleration and boost, and the best handling.
- **Bulwark:** the slowest at the top end but the quickest off the line, with the biggest boost tank, which recharges fastest. Heavy, with very little grip: it slides wide through corners, so it falls off unfenced tracks more easily.

Driven well, all three average the same speed over a lap (within 0.1% across the stock tracks): each wins on some tracks and loses on others.

## Code layout

```
main/Source/
  core/        game logic, no platform or rendering dependencies
    Track        text format parser, centreline, track queries
    Vehicle      craft specs and parser
    Race         RaceSim: fixed 60 Hz simulation of every racer (physics, jumps, falls, laps, positions)
    Controller   who drives a racer: LocalController, AIController (net::RemoteController in net/)
    RaceSetup    everything needed to start the same race anywhere
    Game         menus, race flow, HUD, touch controls
    Sound        sound effects and engine hum, plus music, on a Bleep synth (Libraries/Bleep)
    net/         network seam (see below)
  render/      Jet world renderer (track meshes, craft, chase camera), 2D drawing and font
  platform/    Tactility: display scaling/rotation (PPA on ESP32-P4/S31), input devices (audio output is in Libraries/Bleep)
  main.cpp     app entry: takes over the display from LVGL and runs the game on its own task
```

`core/` and `render/` only use standard C++, so they also build into a desktop harness (used to render the test screenshots and to tune the AI and craft balance).

The game renders at a reduced resolution with the display's aspect ratio, at most 400x240 (640x360 on chips with a PPA). It's then scaled up and rotated to the panel: in hardware on ESP32-P4/S31, in software elsewhere. Monochrome and greyscale panels get an ordered dither.

### Networking (not implemented yet)

The architecture allows races against players on other devices, over whatever transport gets implemented later:

- `RaceSim` is deterministic. Given the same `RaceSetup` (track, laps, seed, slots) and the same per-tick `RacerInput`s, it produces the same race. It doesn't know who drives each racer.
- A racer is driven by a `Controller`. A remote player becomes a `net::RemoteController`, which is fed by received input messages and repeats the last known input when one is late.
- `net::NetSession` is the transport interface: broadcast a message, receive messages. It's the only class a network mode has to implement.
- `net/Protocol.h` defines compact, endian-independent messages:
  - `StartRace`: the host's `RaceSetup`.
  - `Input`: one racer's input for one tick.
  - `Snapshot`: authoritative racer state from the host, to correct floating point drift between chip architectures.

A network race would build its `RaceSetup` in a lobby rather than the menu. Each device turns its own players into `Local` slots and everyone else into `Remote` slots, then runs the same `RaceSim`.

## Sound

Everything is synthesised at runtime, with no samples. Seven voices (pulse, triangle, saw and noise waves) are mixed to mono at 22 kHz: four play music, two play effects, and one is your engine, whose pitch follows your speed.

Songs are Bleep files (`.bleep`, see `Libraries/Bleep`), which you can write with the BleepMixer app or a text editor: `music/menu.bleep`, `music/results.bleep`, and one per track next to its `.trk` file. They're written like a tracker, one bar of 16th notes per block:

```
bpm 120
bar
lead    F#5 --- --- --- --- --- E-5 --- D-5 --- --- --- A-4 --- --- ---
harmony D-4 --- A-4 --- F#4 --- A-4 --- D-4 --- A-4 --- F#4 --- A-4 ---
bass    D-3 --- --- --- --- --- A-2 --- D-3 --- --- --- --- --- A-2 ---
drums   K.. ... ... ... S.. ... ... ... K.. ... K.. ... S.. ... ... H..
```

Each channel has 16 tokens: a note (name, `#` or `-`, octave), `---` to hold the previous note, or `...` for silence. Drums are `K..` (kick), `S..` (snare) and `H..` (hi-hat). Bars play in order and the song loops. A track without a `.bleep` file races in silence.

The game thread only posts to the synth, and an audio task renders it into Tactility's audio stream, so a slow frame never stalls the sound. Devices without audio output run silent.

## Notes

- Jet is built without its ESP32 mode: an ELF-loaded app can't place code in IRAM, so `JetPlatform.h` turns those placement attributes into no-ops.
- `platform/CxxRuntime.cpp` provides `__cxa_atexit` for Jet's static scratch buffers. They're freed when the game exits.
