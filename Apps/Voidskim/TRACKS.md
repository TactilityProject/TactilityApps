# Making tracks for Voidskim

A track is a plain text file in `assets/levels/`, with its music in a `.bleep` file of the same name next to it. Tracks also load from the `levels/` folder in the app's user data directory, so you can add your own without rebuilding the app.

## Files

```
assets/levels/5-my-track.trk    the track
assets/levels/5-my-track.bleep  its music (optional: without it the race is silent)
```

The menu lists tracks in file name order, so a number prefix sets their position. If a track has an error, it's left out of the menu and the error is logged with the line number.

## The format

One command per line. A comment is a `#` at the start of a line, or a `#` followed by a space (`#RRGGBB` colours aren't comments). Distances are in metres and angles in degrees.

### Settings

| Command | Meaning |
|---|---|
| `name Harbor Loop` | Name shown in the menu |
| `laps 3` | Number of laps (1-20) |
| `colour <key> #RRGGBB` | Theme colour (see Colours) |

### Road shape

These lay down the road, in order, from the start line:

| Command | Meaning |
|---|---|
| `straight 120` | Straight road |
| `curve 90 60` | Bend: angle (positive turns right, negative left) and radius |
| `ramp 22 3` | Kicker ramp: length and height. The road drops away at its end, so you jump |
| `gap 18` | No road at all: jump it or fall into the void |

### State

These change the road from the next shape command onwards:

| Command | Meaning |
|---|---|
| `width 16` | Road width (6-60). Eases in over the next segment |
| `height 9` | Road height (0-40). Eases in over the next segment |
| `walls both` | Barriers: `both`, `left`, `right` or `none`. Without a barrier you can fall off |
| `walls both easy` | The same, but only on the listed difficulties (`easy`, `normal`, `hard`) |
| `tunnel on` / `tunnel off` | Covered road: walls on both sides and a roof you can't jump through |

### Colours

`sky` (top of the sky), `horizon`, `below` (the void under the track), `road` and `road2` (alternating bands), `edge` and `edge2` (curb stripes on open edges), `wall`, `ramp`, `structure` (track underside, pillars, tunnels), `scenery` (towers beside the track).

## Rules the loader enforces

- **The lap must close.** The track has to end where it started, facing the same way, and back at height 0.
  - Small position mismatches (up to 10% of the length) are spread out over the lap; larger ones are an error.
  - The curve angles need to add up to 360 (or -360) for a simple loop. A figure-8 style layout adds up to 0.
- **Slopes:** a `height` change can rise at most 0.3 m per metre of the segment it eases in over. For a 9 m bridge, give the climb at least 30 m, and preferably 45 m or more.
- **No level crossings:** if the track crosses itself, one part has to be at least 7 m above the other. Otherwise the loader rejects the track and tells you where, like "the track crosses itself 22 m from the start without a bridge".
- **Minimum radius:** curve radii must be at least 5 m. In practice use at least 40 m (see below).

## Design guidelines

These come from building and playtesting the stock tracks.

### Flow

- **Alternate the turns.** Long stretches turning the same way make you hold one direction for ages, which is tiring and boring. Mix left and right bends, use S-bends, and keep any run of same-direction turning under about 180°.
- **Give it some straight road.** A track of nothing but bends feels the same all the way round. Bends can lead straight into each other, but aim for at least a fifth of the lap as straights, from short links to a long one where boost pays off. Fast, open tracks can have more.
- **Length:** 1.1-2 km laps work well, at about 20-40 s per lap. Shorter feels cramped and longer drags over three laps.

### Bends

- **Radius sets the speed.** The AI, and a good player, take a bend at roughly `turn rate × 0.8 × radius`:
  - 60-100 m bends are taken nearly flat out.
  - 40-50 m bends need a lift or a brake.
  - Below 40 m starts to feel like a hairpin; use them sparingly.
- **Width:** 14-20 m. Narrow tracks (12-14 m) without barriers are hard; save them for the expert tracks.

### Edges and barriers

- **Open edges are the danger.** A track without barriers is tense, and falling off costs about 4 seconds (the fall, the respawn and losing speed).
- **Barriers can help beginners.** `walls both easy` before a tricky bend and `walls none easy` after it give Easy players a safety net, without changing the track for everyone else.

### Jumps

A jump should feel like leaping an abyss you could fall into. The game draws deep rock cliffs on both sides of every `gap`, with a hazard stripe along each lip, so the gap reads as a chasm. Sizing and layout do the rest:

- **Use short, low ramps.** Launch power comes from the ramp's slope (height ÷ length), not its height. `ramp 8 1.6` launches as hard as `ramp 20 4`, but its low lip doesn't hide the far side: you see the abyss and the landing cliff as you come up to it.
- **Keep take-off and landing level.** Don't raise the run-up to jump down to a lower landing. From the chase camera, anything lower than the lip is hidden, so the landing disappears.
- **Make the gap long enough to miss.** Size it so you need roughly 55-60% of top speed to clear it (about 35-42 m/s): flat out you fly well over, but slowed down by a wall hit or a bad line, you fall short. Too short a gap is a bump, not a jump.
- **Leave room to land.** Boosting craft fly a lot further. Give 50-70 m or more of straight road after the gap before the next bend.
- **Give it a straight run-up.** At least 20 m of straight before the ramp: jumping out of a bend tends to land you off the edge.

Approximate distance flown from the lip of a level jump (arcade gravity is 30 m/s²):

| Ramp (slope) | 30 m/s | 40 m/s | 50 m/s | 65 m/s | 80 m/s (boost) |
|---|---|---|---|---|---|
| `ramp 8 1.6` (0.2) | 17 m | 28 m | 40 m | 63 m | 93 m |
| `ramp 8 1.8` (0.225) | 19 m | 30 m | 44 m | 71 m | 103 m |
| `ramp 8 2` (0.25) | 21 m | 33 m | 49 m | 78 m | 114 m |

Suggested gaps: 22 m on an easy track, 26-30 m on a normal one, 30-34 m for experts.

### Crossings, bridges and tunnels

- **Raise one part 9 m.** A tunnel's roof is about 6.6 m up, and a bridge's underside is 1.6 m below its road, so 9 m leaves a little clearance.
- **Put the lower part in a tunnel.** Turn `tunnel on` about 20-25 m before the crossing and `tunnel off` about 20-25 m after it. It frames the moment nicely.
- **Cross at a good angle.** 60-90° crossings read well. Shallow crossings run the two roads alongside each other for a long stretch, which looks messy and confuses the eye.
- **Keep parts apart.** Apart from at crossings, keep different parts of the track at least about 25 m apart (centre to centre), or the roads visually merge.
- **Space the climbs.** Leave the climb and descent room: crossings visited close together along the lap force steep slopes.

### Starting line

- **Start on a straight.** The grid lines up behind the start line, back to about 30 m before it with five opponents. Keep that stretch straight and free of ramps, gaps and climbs.

### Colours

- **Contrast:** keep good contrast between `road`/`road2` and `edge`/`edge2`. The edge stripes are how players see where they can fall off.
- **Mood:** `sky` and `horizon` set the mood (sunset, dusk, night), and `below` should be dark, because it's the void.

## Checking a track

1. **Open it in the menu.** If it's missing, check the log for the line number and error.
2. **Watch the preview.** The menu circles the start line, which shows the colours and the grid area.
3. **Race it on Easy and Hard.** If the AI falls off a lot on Hard, a bend is probably too tight for an open edge, or a gap too long.
4. **Look at the minimap.** It shows the whole layout, with gaps in red.
5. **Jump slowly once.** Take each jump at about half speed: you should fall short. If you clear it anyway, lengthen the gap.

## Example

A small loop with a jump, and barriers on one bend for Easy only:

```
# Practice Ring
name Practice Ring
laps 3
width 18
walls none
colour sky #0B3D91
colour horizon #FF9E5E

straight 60
ramp 8 1.6
gap 24
straight 78
curve 90 60
straight 40
walls both easy
curve 90 60
walls none easy
straight 200
curve 90 60
straight 40
curve 90 60
straight 30
```

It closes: the long sides are 60 + 8 + 24 + 78 + 30 = 200 m going out and 200 m coming back, the short sides are both 40 m, and the four 90° bends of the same radius add up to 360°. The final 30 m straight is where the grid lines up.
