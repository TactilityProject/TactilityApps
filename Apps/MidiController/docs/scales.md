# Scales and Keys

Defined in `MidiScales.h`. The shared scale/key helpers are used for note generation by RNG, ZEN, DROP, and CHORD (CHORD always uses a C root). KEYS keeps a scale/key selection for its status row but remains fully chromatic. TB3PO has its own internal six-scale table; RAGA uses raga-specific pitch sets.

---

## Scales

| Index | Name | Intervals (semitones from root) | Notes |
|-------|------|---------------------------------|-------|
| 0 | Major | 0 2 4 5 7 9 11 | 7 |
| 1 | Minor (natural) | 0 2 3 5 7 8 10 | 7 |
| 2 | Dorian | 0 2 3 5 7 9 10 | 7 |
| 3 | Pentatonic | 0 2 4 7 9 | 5 |
| 4 | Blues | 0 3 5 6 7 10 | 6 |
| 5 | Chromatic | 0 1 2 3 4 5 6 7 8 9 10 11 | 12 |

---

## Keys

12 root notes, index 0–11:

`C  C#  D  D#  E  F  F#  G  G#  A  A#  B`

The key is a transpose offset applied on top of the scale intervals. Key 0 = C, Key 2 = D, etc.

---

## Helper functions

### `getNoteInScale(scaleIdx, degree, octave, keyTranspose)`
Returns the MIDI note number for a given scale degree, octave, and key. `degree` wraps around the scale length automatically.

```
getNoteInScale(0, 0, 4, 0) → 60  (C4, Major scale, degree 0)
getNoteInScale(0, 1, 4, 0) → 62  (D4, Major scale, degree 1)
getNoteInScale(3, 0, 4, 2) → 62  (D4, Pentatonic scale, root=D)
```

### `isNoteInScale(scaleIdx, keyTranspose, midiNote)`
Returns `true` if the note belongs to the given scale/key. Available for scale-aware highlighting; the current KEYS view is fully chromatic.

### `isRootNote(keyTranspose, midiNote)`
Returns `true` if the note is the root (tonic) of the key. GRID is chromatic and highlights C notes directly.

### `midiNoteName(note, buf, len)`
Formats a MIDI note as `"C#4"`, `"A#3"`, etc. All accidentals use sharps: C#, D#, F#, G#, A#. MIDI 60 is C4 (scientific pitch notation).

### `midiNoteClass(note, buf, len)`
Returns only the pitch class without octave: `"C#"`, `"G"`, etc.

---

## Scale usage by mode

| Mode | Uses scale | Notes |
|------|-----------|-------|
| KEYS | Label only | Fully chromatic; scale/key shown in the status row but not applied to key colours or pitches |
| BEATS | No | Fixed drum notes |
| ZEN | Yes | Wall segments map to scale degrees |
| DROP | Yes | Platform note derived from scale |
| RNG | Yes | Random note drawn from scale |
| XY | No | Raw CC values |
| ARP | No | Chromatic piano plus fixed major/minor/7th chord intervals |
| GRID | No | Chromatic isomorphic layout; C notes highlighted |
| CHORD | Yes | Roots from scale with fixed C key; chord qualities follow the major-scale degree pattern |
| LFO | No | CC/pitch bend only |
| RAGA | Custom | Raga-specific pitch sets (not using scale index) |
| DIMENSIONS | No | Equation output mapped to MIDI range |
| BABY8 | No | Notes set freely per step |
| TB3PO | Yes | Own six-scale table; generated steps quantised to scale (its Chrome option is the full 12-note chromatic scale) |
| EUCLID | No | Notes set freely per voice |
