# 8Encoder

The M5Stack 8Encoder V2 unit normally connects via **PaHub CH4**, though the app probes every PaHub channel and accepts it on any port. It provides 8 rotary encoders with push-buttons, individually addressable RGB LEDs, and one toggle switch with its own LED.

The encoder is completely optional. If the PaHub or the encoder unit is not detected, the app operates in touch-only mode with no degradation.

---

## Per-mode mappings

Only five modes use the encoder. In all other modes the encoder is idle (LEDs off).

### RAGA

| Control | Action |
|---------|--------|
| Knob 0 | Raga selection (0–7, wraps) |
| Knob 1 | Tala selection (0–2, wraps) |
| Knob 2 | Root note (MIDI 24–96) |
| Knob 3 | BPM (40–240) |
| Knobs 4–7 | Unused |
| Button 0 | Play / Stop toggle |
| Button 1 | Drone On / Off toggle |
| Buttons 2–7 | Unused |

**LED colours:**

| Knob | Colour | Meaning |
|------|--------|---------|
| 0 | Orange `0xFF8800` | Raga |
| 1 | Deep orange `0xFF6600` | Tala |
| 2 | Cyan `0x00FFCC` | Root |
| 3 | Yellow `0xFFE000` | BPM |
| 4–7 | Off | — |

---

### DIMENSIONS

| Control | Action |
|---------|--------|
| Knob 0 | Parameter A (0–100) |
| Knob 1 | Parameter B (0–100) |
| Knob 2 | Parameter C (0–100) |
| Knob 3 | Parameter D (0–100) |
| Knob 4 | Equation (1–20) |
| Knob 5 | BPM (40–280) |
| Knob 6 | Octave offset (−4 to +4) |
| Knob 7 | Random amount (0–64) |
| Button 0 | Play / Stop toggle |
| Button 1 | Reset (A/B/C/D → 50, t → 0) |
| Buttons 2–7 | Unused |

**LED colours** (match the UI label colours):

| Knob | Colour | Parameter |
|------|--------|-----------|
| 0 | `0x88DDFF` (light blue) | A |
| 1 | `0x88FFCC` (mint) | B |
| 2 | `0xFFCC88` (peach) | C |
| 3 | `0xFF88CC` (pink) | D |
| 4 | `0xFF8844` (orange) | Equation |
| 5 | `0xFFE000` (yellow) | BPM |
| 6 | `0xAAFF88` (lime) | Octave |
| 7 | `0xFFAA44` (amber) | Random |

---

### TB3PO

| Control | Action |
|---------|--------|
| Knob 0 | Density (0–16) |
| Knob 1 | BPM (40–280) |
| Knob 2 | Key root (0–11, wraps) |
| Knob 3 | Octave (1–6) |
| Knob 4 | Scale selection (6 options, wraps) |
| Knob 5 | Regen (any delta draws a new random seed) |
| Knobs 6–7 | Unused |
| Button 0 | Play / Stop toggle |
| Button 1 | Regen (new random seed and pattern) |
| Buttons 2–7 | Unused |

**LED colours:** While playing, animate with the current 32-step playback position. The steps are grouped into eight 4-step ranges: knob 0 = steps 1–4, knob 1 = steps 5–8, … knob 7 = steps 29–32. The knob for the active range is lit orange `0xFF6600`; the other seven are dark `0x221100`. All eight are dark while stopped.

---

### BABY8

| Control | Action |
|---------|--------|
| Knob 0 | Step 1 note (0–127) |
| Knob 1 | Step 2 note |
| Knob 2 | Step 3 note |
| Knob 3 | Step 4 note |
| Knob 4 | Step 5 note |
| Knob 5 | Step 6 note |
| Knob 6 | Step 7 note |
| Knob 7 | Step 8 note |
| Button 0 | Toggle gate for step 1 |
| Button 1 | Toggle gate for step 2 |
| … | … |
| Button 7 | Toggle gate for step 8 |

**LED colours:**
- Current playback step (while playing): white `0xFFFFFF`
- Gate on (not current): dark green `0x004400`
- Gate off: dark red `0x440000`

---

### EUCLID

| Control | Action |
|---------|--------|
| Knob 0 | Selected voice — Steps (1–32) |
| Knob 1 | Selected voice — Events (0–steps) |
| Knob 2 | Selected voice — Rotation (0–steps-1, wraps) |
| Knob 3 | Selected voice — Note (0–127) |
| Knob 4 | BPM (40–280) |
| Knob 5 | Voice 6 — Steps |
| Knob 6 | Voice 7 — Steps |
| Knob 7 | Voice 8 — Steps |
| Button 0 | Select voice 1 |
| Button 1 | Select voice 2 |
| … | … |
| Button 7 | Select voice 8 |

**LED colours** (per-voice accent colours):

| Knob/Button | Colour | Voice |
|-------------|--------|-------|
| 0 | `0xFF4040` Red | Voice 1 |
| 1 | `0xFF9900` Orange | Voice 2 |
| 2 | `0x44DD44` Green | Voice 3 |
| 3 | `0x44BBFF` Blue | Voice 4 |
| 4 | `0xFF44FF` Magenta | Voice 5 |
| 5 | `0xFFDD00` Yellow | Voice 6 |
| 6 | `0x00FFCC` Cyan | Voice 7 |
| 7 | `0xFF8888` Pink | Voice 8 |

Selected voice LED is at full brightness; all others are dimmed to ¼ intensity (`>> 2` each channel).

---

## Button edge detection and mode-switch safety

Buttons use a rising-edge detector: `if (btn && !btnPrev[i])`. This means holding a button across a mode switch will not register as a press in the new mode.

On each mode switch `btnSeedNext` is set. The very next encoder poll reads the live button state silently into `btnPrev` without firing any actions, then returns. This seeds the edge detector correctly regardless of which buttons are physically held.

---

## Poll rate

The encoder poll timer fires every **100 ms** (10 Hz). This is a balance between responsiveness and I2C bus traffic: each poll requires up to 16 individual I2C reads (8 deltas + 8 buttons) across the PaHub multiplexer.

Delta values accumulate in the encoder hardware between polls, so fast knob turns are not lost — the delta register counts all movement since the last read.
