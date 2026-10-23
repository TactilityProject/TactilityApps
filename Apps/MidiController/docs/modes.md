# Modes

The home screen shows a 3×5 grid of tiles. Tapping any tile switches to that mode.
Tap the **←** back button (top-left of most views) to return home.

---

## KEYS — Chromatic Piano

Single octave of piano keys. Drag your finger across keys for continuous glide. Multiple fingers play independently; each finger tracks its own note.

**Parameters:**
- Octave (1–8) — base octave of the keyboard
- Scale / key root — shown in the status and control rows only; the keyboard stays fully chromatic and is not transposed

**Controls:** OCT− / OCT+ buttons; SCALE cycle; KEY− / KEY+

**MIDI:** Note On on finger contact, Note Off on release. Each touch slot holds and slides its own note; lifting one finger does not release the others.

**Scale/key controls** use the same scale table as RNG, ZEN, and DROP, but KEYS keeps its own selection and does not apply it to the keyboard. CHORD uses the same scale table with its key fixed at C — see [scales.md](scales.md).

---

## BEATS — 4-Track Drum Sequencer

Classic 4×16 step sequencer. Steps loop continuously while playing.

**Tracks:**
| Track | Default MIDI note | Colour |
|-------|------------------|--------|
| KICK | 36 (C2) | Red |
| SNARE | 38 (D2) | Yellow |
| HIHAT | 42 (F#2) | Cyan |
| OPEN | 46 (A#2) | Purple |

**Controls:** Tap any step cell to toggle it. PLAY/STOP, CLEAR (stops playback and turns all steps off), BPM− / BPM+.

**Note duration:** Each track has a fixed note-off delay (KICK 200 ms, SNARE 150 ms, HIHAT 50 ms, OPEN 300 ms).

---

## ZEN — Ambient Physics

Up to 4 balls bounce around a rectangular arena. Each wall segment triggers a scale note on collision.

**Wall layout:** 8 top segments, 8 bottom segments, 4 left segments, 4 right segments (24 total). Each segment maps to a scale degree; walls flash white briefly on hit.

**Controls:** ADD (spawn ball), RESET (remove all but one), SCALE cycle, KEY± , OCT.

**Parameters:** Scale, key, octave (2–6), ball count (1–4).

**MIDI:** Random velocity (70–109). Note Off follows 150 ms after each Note On; walls flash for 200 ms on impact.

---

## DROP — Gravity Platform Sequencer

Balls roll and fall under gravity, triggering notes when they land on platforms.

Two modes — **EDIT** (tap to place horizontal platforms) and **DROP** (tap to spawn balls).

**Controls:** Toggle EDIT/DROP, CLEAR (removes balls and platforms), scale/key/octave.

**Parameters:** Scale, key, octave (2–7), up to 6 platform positions. Platforms are always horizontal and receive a note automatically from the selected scale by platform index (degrees 0, 2, 4, 6, 1, 3).

---

## RNG — Random Melody Generator

Generates random melodies constrained to a scale, key, octave range, and probability.

**Parameters:**
| Parameter | Range | Default |
|-----------|-------|---------|
| BPM | 40–300 | 120 |
| Subdivision | ¼ / ⅛ / 1/16 | ¼ |
| Min octave | 1–8 (≤ max) | 3 |
| Max octave | 1–8 (≥ min) | 6 |
| Probability | 0–100 % | 50 % |
| Scale | 6 options | Major |
| Key | C–B | C |

**Controls:** PLAY/STOP, BPM± (step 5), KEY±, SCALE cycle, PROB± (step 5), MinOct±, MaxOct±, Subdivision cycle.

On each tick, a note is chosen randomly from `(scale, key, octave range)` with the given probability. It plays for 75 % of the step duration, then Note Off.

---

## XY — 2D CC Pad

Full-screen touch pad. Finger X maps to one CC, finger Y maps to another.

**Parameters:**
| Axis | Default CC | Suggested use |
|------|-----------|---------------|
| X | 74 | Filter cutoff |
| Y | 71 | Resonance |

**Controls:** X CC± / Y CC± buttons to change target CC numbers (0–127).

MIDI CC is sent continuously while the finger is held. Values range 0–127 across the full pad width/height.

---

## ARP — Chord Arpeggiator

Touch a note on the mini piano to latch an arpeggio of the selected chord type at the selected pattern and speed.

**Chord types:** Major, Minor, Dominant 7th.

**Patterns:** UP, DOWN, UP-DOWN, RANDOM, CHANCE (probabilistic skips).

**Speed:** Quarter / Eighth / Sixteenth / 32nd note.

**Parameters:** BPM, arpeggio pattern, chord type, octave range (1–4 octaves), piano octave (1–7).

**Controls:** Tap piano key (bottom row) to start arp. Pattern/Chord/OctRange/Speed/BPM controls at top.

---

## GRID — Isomorphic Grid

8 columns × 5 rows. Each row is offset by a perfect 4th (5 semitones) from the row below — the same layout as a Linnstrument. Every cell adds one semitone across the row.

**Note layout:**
```
Row 4 (top): root + 20 … 27 semitones
Row 3:       root + 15 … 22
Row 2:       root + 10 … 17
Row 1:       root +  5 … 12
Row 0 (bot): root +  0 …  7   (chromatic: C, C#, D, D#, E, F, F#, G at octave 3)
```

**Cell colours:** C notes bright cyan; black-key notes dark blue; white-key notes medium blue; pressed cells bright cyan.

**Controls:** OCT− / OCT+ (octave 1–6).

Cells show the note name. Touch for Note On, release for Note Off. Multi-touch is supported: each pointer slot tracks its own held cell, so lifting one finger does not release notes held by the others.

---

## CHORD — Diatonic Chord Player

8 large buttons play chords built on the selected scale, with the root fixed at C. Chord VIII is chord I one octave up.

**Chord voicings:** MAJ (root+M3+P5), MIN (root+m3+P5), DIM (root+m3+dim5). The chord qualities follow the major-scale degree pattern: I/IV/V major, ii/iii/vi minor, vii diminished, I+ major. The selected scale supplies the chord roots.

**Controls:** Press a chord button to play; release to stop. OCT± / SCALE cycle / CLEAR.

**Parameters:** Octave (1–7), scale. The key is fixed at C.

---

## LFO — Low-Frequency Oscillator

Continuously modulates a MIDI CC (or pitch bend) with a selectable waveform.

**Waveforms:** Sine, Triangle, Square, Sawtooth.

**Parameters:**
| Parameter | Range | Default |
|-----------|-------|---------|
| Rate | 0.1–10 Hz | 1 Hz |
| Amount | 0–127 | 64 |
| CC target | 0–127 | 1 (modwheel) |
| Mode | CC / Pitch | CC |

**Controls:** START/STOP, Rate±, WAVE cycle, Amount±, CC±, PITCH mode toggle.

In **Pitch** mode, a 14-bit pitch-bend message centred at 8192 is used instead of a CC. The actual semitone range depends on the receiver's pitch-bend setting (commonly ±2 semitones). The waveform is drawn live on the canvas.

---

## RAGA — Indian Classical Generator

Generates melodic phrases based on 8 classical Indian ragas with tala (rhythmic cycle) structure and an optional drone note.

**Ragas:**
| # | Name | Character |
|---|------|-----------|
| 0 | Bhairavi | Morning, devotional |
| 1 | Lalit | Pre-dawn |
| 2 | Bhupali | Evening, peaceful |
| 3 | Todi | Morning, intense |
| 4 | Madhuvanti | Afternoon |
| 5 | Meghmalhar | Monsoon |
| 6 | Yaman | Evening, romantic |
| 7 | Malkauns | Night, serious |

**Talas:**
| # | Name | Beats |
|---|------|-------|
| 0 | Teental | 16 |
| 1 | Rupak | 7 |
| 2 | Jhaptal | 10 |

**Parameters:** Raga (0–7), tala (0–2), root note (MIDI 24–96), BPM (40–240).

**Controls:** RAGA±, TALA±, ROOT±, BPM±, PLAY/STOP, DRONE toggle.

**How it works:** Bhairavi phrases use a 2nd-order Markov chain; the other seven ragas use a degree random walk. A new pitch is emitted every 16th note (BPM/4), while the selected tala supplies the beat-accent cycle. The drone note (root) sustains independently of the melody.

**Encoder:** Knob 0=raga, 1=tala, 2=root, 3=BPM; Button 0=play/stop, Button 1=drone.

---

## DIMENSIONS — Parametric Equation Sequencer

20 parametric equations map 4 parameters (A, B, C, D) and a running time variable (t) to a 3D position (px, py, pz). The position is quantised to a MIDI note each tick.

**Parameters:**
| Param | Range | Description |
|-------|-------|-------------|
| Equation | 1–20 | Selects the formula |
| A, B, C, D | 0–100 | Equation coefficients |
| Octave | −4 to +4 | Note range offset |
| Random | 0–64 | Adds stochastic perturbation |
| BPM | 40–280 | Tick rate |

**Controls:** Equation±, PLAY/STOP, RESET (resets A/B/C/D to 50 and t to 0), BPM±, OCT±, Rnd±.

**Equation variety:** Includes Lissajous figures, chaotic attractors, Fourier synthesis, and other 3D parametric forms. Each produces a different melodic contour as t advances.

**Encoder:** Knobs 0–3 = A/B/C/D; Knob 4 = equation; Knob 5 = BPM; Knob 6 = octave; Knob 7 = random; Button 0 = play/stop; Button 1 = reset.

---

## BABY8 — 8-Step Melodic Sequencer

Classic Baby-8 / analogue-sequencer style. 8 steps, each with a note, velocity, and gate. Up to 8 patterns.

**Step display:**
| State | Colour |
|-------|--------|
| Current playing step, gate on | White |
| Current playing step, gate off | Blue |
| Selected (not playing), gate on | Orange |
| Selected (not playing), gate off | Brown |
| Other step, gate on | Dark green |
| Other step, gate off | Dark grey |

**Parameters per step:** Note (0–127), velocity (0–127), gate (on/off).

**Global parameters:** BPM (40–280), swing (50–80 %), active pattern (1–8, stored as index 0–7).

**Controls:** PLAY/STOP, BPM±, SWING±, tap step to select, NOTE±, VEL±, GATE toggle, PATTERN±.

**Swing:** Lengthens even-numbered steps and shortens odd-numbered steps by the same amount, so the pair still fills two 16th notes and the tempo stays constant. At 50 % there is no swing; at 80 % the groove is heavy.

**Encoder:** Knobs 0–7 = step note (by index); Buttons 0–7 = toggle step gate.

---

## TB3PO — 303-Style Acid Bass

Procedurally generated 32-step bass patterns in the spirit of the Roland TB-303.

**Step properties:** Note degree, octave shift (up/down), accent (higher velocity), slide (legato transition into the next gated step), gate (active/silent).

**Parameters:**
| Parameter | Range | Default |
|-----------|-------|---------|
| Density | 0–16 | 8 |
| BPM | 40–280 | 120 |
| Key | C–B | C |
| Octave | 1–6 | 3 |
| Scale | 6 options | Major |
| Seed | uint32 (randomized on REGEN) | 12345 |

**Controls:** PLAY/STOP, REGEN (new random seed and pattern), Density±, BPM±, KEY±, SCALE cycle. Octave has no touch control; use encoder knob 3.

**Pattern generation (REGEN):** An LCG PRNG seeded by the current seed value generates note degrees, gate probability (higher density = more gates), accents, and slides. REGEN and density changes draw a new random seed; key and scale changes reuse the current seed. The same seed and parameters always produce the same pattern.

**Slide:** A slide flag holds the step's note past its nominal 90%-gate Note Off. If the next step is gated, that step's Note On is sent before the old note's Note Off, so a mono synth hears a legato transition. The old note is then released so a poly synth does not leave it hanging. If the next step is silent, the held note ends at the step boundary.

**Encoder:** Knob 0=density, 1=BPM, 2=key, 3=octave, 4=scale, 5=regen (any delta triggers regen); Button 0=play/stop, Button 1=regen.

---

## EUCLID — 8-Voice Euclidean Sequencer

8 independent voices, each with a Euclidean rhythm pattern. All voices share a global BPM and step clock. The canvas shows 8 concentric rings (one per voice) with a rotating sweep line indicating the current position.

**Euclidean patterns:** A bucket (Bresenham-style) accumulator distributes `events` triggers as evenly as possible across `steps` total slots. Before rotation the first trigger lands on step 0; rotation shifts the whole pattern. For example, E(3,8) with rotation 0 produces `[1,0,0,1,0,0,1,0]` (triggers on steps 0, 3, and 6).

**Per-voice parameters:**
| Parameter | Range | Description |
|-----------|-------|-------------|
| Steps | 1–32 | Total pattern length |
| Events | 0–steps | Number of triggers |
| Rotation | 0–steps-1 | Pattern phase offset |
| Note | 0–127 | MIDI note triggered |

**Global parameters:** BPM (40–280), triplet mode (2/3 timing).

**Voice colours:** Each voice has a distinct colour both on the canvas ring and its tab button.

**Controls:** PLAY/STOP, TRIPLET toggle, BPM±; per-voice: tap voice tab to select, Steps±, Events±, Rotation±, Note±.

**Encoder:** Knob 0=sel.steps, 1=sel.events, 2=sel.rotation, 3=sel.note, 4=BPM; Knobs 5–7=steps for voices 6–8; Buttons 0–7 = select voice.
