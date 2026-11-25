# BleepMixer

A chiptune tracker for Tactility devices. It writes songs in the Bleep format (`.bleep`), the music of Voidskim, so you can compose a soundtrack and drop it next to a Voidskim track.

## Features

- **Four channels**: lead (25% pulse), harmony (12.5% pulse), bass (triangle) and drums (kick, snare, hi-hat)
- **Bars of 16 steps**: add, copy and delete bars; the song plays them in order and loops
- **Tempo** from 40 to 300 BPM
- **Playback** of the whole song (the view follows it) or of the current bar in a loop, with per-channel mute
- **Note entry** from a keyboard laid out like a piano, or the on-screen keypad on touch screens
- **Songs** are plain text, saved in the app's user data directory under `songs/`. Voidskim's soundtrack comes with it as examples.

## Screen

- **Toolbar**: open, save, save as, show or hide the keypad, play/stop.
- **Bar row**: previous/next bar, add an empty bar after this one, copy this bar, delete it, loop this bar, tempo.
- **Pattern**: the 16 steps of the bar for the four channels. Tap a step to move the cursor there (tap it again to hear it), drag to scroll, tap a channel name to mute it.
- **Keypad** (touch): the 12 notes of the octave, octave down/up, `---` (hold), `...` (silence) and the three drums.

## Keys

| Key | Does |
| --- | --- |
| Arrows | move the cursor (down from the last step continues in the next bar, up from the first one goes back a bar) |
| `z s x d c v g b h n j m` | notes C to B in the current octave |
| `q 2 w 3 e r 5 t 6 y 7 u` | notes C to B an octave up |
| `z`/`k`, `x`/`s`, `c`/`h` | kick, snare, hi-hat (in the drums channel) |
| `-` | hold the previous note (`---`) |
| `.`, Backspace, Delete | silence (`...`) |
| `[` `]` | octave down/up |
| `,` `/` (or `<` `>`) | previous/next bar |
| Space | play/stop |
| Enter | hear the step under the cursor |
| Up on the first step, Shift+Tab | from the pattern to the bar buttons (arrows move between them, Enter presses one; down past the last row, or Tab, returns to the pattern) |

Entering a step plays it and moves the cursor down.

## Files

Songs are saved as `<name>.bleep` in `songs/` in the app's user data directory. Open lists your songs, then the examples; an example you change is saved as a song of your own.

The format is described in [Libraries/Bleep](../../Libraries/Bleep/README.md). To use a song in Voidskim, name it after a track (`5-my-track.bleep` next to `5-my-track.trk`).
