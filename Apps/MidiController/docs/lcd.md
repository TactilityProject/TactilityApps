# LCD Unit Display

The M5Stack LCD Unit (135×240 IPS, portrait) normally connects via **PaHub CH5**, though the app probes all six PaHub channels and accepts it on any port. It shows a live secondary display that mirrors and extends the current mode's state.

The LCD is completely optional. If the PaHub or the LCD unit is not detected, `lcdUpdate` is a no-op throughout.

---

## General layout

The display is **135 px wide × 240 px tall** in portrait orientation (rotation 0).

Every layout starts with a **16 px coloured header bar** (plus a 1 px separator) at the top. For the five encoder modes it shows the mode name in 2× scale text, using that mode's accent colour; other modes show a generic `MIDI` header.

A **5×7 bitmap font** (printable ASCII 32–126) is used for all text rendering. Two scale factors are used:
- **1× scale** — 5×7 px glyphs (very compact, for labels and values)
- **2× scale** — 10×14 px glyphs (for names and primary values)

Colours are RGB565. The background is always black (`0x0000`).

---

## Per-mode displays

### HOME / Other non-encoder modes

```
┌─────────────────┐
│      MIDI       │  ← 2× text on grey background (16 px)
│                 │
│                 │
│  (blank)        │
└─────────────────┘
```

Shows the generic `MIDI` header. The content area is blank (these modes don't use the encoder or LCD detail view).

---

### RAGA

```
┌─────────────────┐  y=0
│      RAGA       │  header (16 px, orange)
│                 │
│   Bhairavi      │  ← raga name, 2× orange (y=22)
│                 │
│ TALA  Teental   │  y=68
│ ROOT  C3   120  │  y=82  (root + BPM on same row)
├─────────────────┤  y=98
│    PLAYING      │  ← red/dark 28 px bar (y=100)
│    DRONE ON     │  ← blue/dark 20 px bar (y=132)
│                 │
│ K1  RAGA        │  ← knob legend (1× text, y=170)
│ K2  TALA        │
│ K3  ROOT        │
│ K4  BPM         │
│ B1  PLAY        │
│ B2  DRON        │
└─────────────────┘
```

**Dirty fields:** Raga name, tala, root note, BPM, play state, drone state — each redraws only when its value changes.

---

### DIMENSIONS

```
┌─────────────────┐
│      DIM        │  header (cyan)
│ A 50     B 50   │  ← labels + values, y=20
│ [████]   [████] │  ← filled colour bars, y=30–38
│ C 50     D 50   │  ← y=50
│ [████]   [████] │  ← bars, y=60–68
│ EQ 3     BPM 120│  ← y=82
│ OCT +0   RND 0  │  ← y=96
├─────────────────┤  y=110
│    STOPPED      │  ← green=playing, dark=stopped (y=112)
└─────────────────┘
```

A/B share the top row (x=2 and x=67); C/D share the next row. Each has a colour bar showing the 0–100 value as a filled rectangle.

**Bar colours** match the encoder LED and UI label colours: A=`#88DDFF`, B=`#88FFCC`, C=`#FFCC88`, D=`#FF88CC`.

---

### TB3PO

```
┌─────────────────┐
│     TB-3PO      │  header (green)
│ ┌──┬──┬──┬──┐   │  ← 32-step piano roll (2 rows of 16)
│ │  │██│  │  │…  │     each step: 6×12 px cell
│ └──┴──┴──┴──┘   │     white=current (while playing), green=gate on, dark=no gate
│                 │     yellow accent dot = accent step
│ KEY C  SCALE Major│
│ DEN 8  BPM  120 │
├─────────────────┤
│    STOPPED      │
└─────────────────┘
```

The piano roll starts at y=20, with two rows on a 15 px pitch (cells are 6×12 px). Row 0 = steps 0–15, row 1 = steps 16–31. Each cell has a 2 px gap between neighbours. Accent steps show a small yellow dot at the top-left corner.

---

### BABY8

```
┌─────────────────┐
│     BABY8       │  header (magenta)
├─────────────────┤
│ 1  C4    [■]    │  ← step row 27 px tall; note name 2×, gate dot
│ 2  D4    [■]    │     white bg = current step (gate on)
│ 3  E4    [ ]    │     white bg + dark dot = current step (gate off)
│ 4  F4    [■]    │
│ 5  G4    [■]    │
│ 6  A4    [■]    │
│ 7  B4    [■]    │
│ 8  C5    [■]    │
└─────────────────┘
```

Each step gets 25 px of height (27 px total with a 2 px separator line). The step number is on the left, note name (2×) in the centre-left, and a filled gate dot on the right. While playing, the current step has a white background with black text; when its gate is off the gate dot is dark instead of green. No step is highlighted while stopped.

---

### EUCLID

```
┌─────────────────┐
│     EUCLID      │  header (red)
│ [1][2][3][4][5][6][7][8]  ← voice tabs y=18–33, 15×15 px
│     VOICE 3     │  ← selected voice name, 2× in voice colour (y=38)
│                 │
│ STEPS    8      │  ← y=80
│ EVENTS   5      │  ← y=94
│ ROT      2      │  ← y=108
│ NOTE     C2     │  ← y=122
│ BPM      120    │  ← y=140
├─────────────────┤  y=154
│    STOPPED      │  ← y=156–174
│ K1  STEPS       │  ← knob legend y=176, 10 px rows
│ K2  EVENTS      │
│ K3  ROT         │
│ K4  NOTE        │
│ K5  BPM         │
│ B   VOICE       │
└─────────────────┘
```

Voice tabs start at y=18 with a 17 px pitch. The selected voice tab is drawn at full colour; others are dark grey. When the voice changes, the name, all parameter values, and the knob legend are all redrawn.

---

## Dirty-caching

`lcdUpdate` is called from the 100 ms encoder poll for the five encoder modes, and immediately on mode switch. Each mode maintains a cache of the last-drawn values. Only fields that have changed are redrawn, keeping bus traffic low.

On a mode switch `lcdSetMode` clears the per-mode cache structs, seeds their change sentinels, and sets `s_dirty = true`, forcing a full screen redraw on the next `lcdUpdate` call.
