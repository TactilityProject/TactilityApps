# Bleep

A small chiptune format and player for Tactility apps, shared by Voidskim (which plays it) and BleepMixer (which writes it).

- `Song.h`: the `.bleep` text format: parse, write, cell helpers
- `Synth.h`: up to 8 voices of pulse, triangle, saw and noise waves with pitch slides and fades, mixed to mono at 22050 Hz. Integer maths only.
- `Sequencer.h`: plays a `Song` on four voices of a `Synth`
- `Player.h`: a ready-made, thread-safe music player (song, loop, mutes, single notes)
- `AudioSource.h`, `AudioOutput.h`: plays anything that renders samples through Tactility's audio stream, on a task of its own

Everything except `AudioOutput` is plain C++17, so it also builds and runs on a desktop (for tests or tools).

## The format

```
# A comment: '#' at the start of a line, or followed by a space
bpm 120

bar
lead    F#5 --- --- --- --- --- E-5 --- D-5 --- --- --- A-4 --- --- ---
harmony D-4 --- A-4 --- F#4 --- A-4 --- D-4 --- A-4 --- F#4 --- A-4 ---
bass    D-3 --- --- --- --- --- A-2 --- D-3 --- --- --- --- --- A-2 ---
drums   K.. ... ... ... S.. ... ... ... K.. ... K.. ... S.. ... ... H..
```

- `bpm`: tempo, 40-300. A row is a 16th note.
- `bar`: starts a bar of 16 rows. Bars play in order, then the song loops.
- `lead`, `harmony`, `bass`: 16 tokens each: a note (`C-4`, `F#5`: name, `-` or `#`, octave 0-8), `---` to let the previous note ring on, or `...` for silence. Channels a bar leaves out are silent.
- `drums`: `K..` kick, `S..` snare, `H..` hi-hat, or `...`.

Lead is a 25% pulse, harmony a thin 12.5% pulse that fades within a row, bass a triangle; notes fade out over a few rows like a pluck.

## Using it

Add the sources and include directory in the app's `main/CMakeLists.txt`:

```cmake
set(BLEEP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../Libraries/Bleep)
file(GLOB BLEEP_SOURCES ${BLEEP_DIR}/Source/*.cpp)
tactility_component_register(SRCS ${SOURCE_FILES} ${BLEEP_SOURCES} INCLUDE_DIRS "${BLEEP_DIR}/Include")
```

Play a song:

```cpp
#include <Bleep/AudioOutput.h>
#include <Bleep/Player.h>

bleep::Song song;
std::string error;
if (bleep::parseSong(text, song, error)) {
    player.play(&song); // song and player must outlive playback
}
audio.start(&player); // bleep::AudioOutput: until audio.stop()
```

For music together with sound effects (as Voidskim does), drive a `Synth` and a `Sequencer` from your own `AudioSource`: the sequencer uses voices 0-3, the rest are yours.

## Threads

`Player` is controlled from one thread and rendered from the audio task. The audio task never blocks: while `lock()` is held (needed only to add or remove bars, or change the tempo, of the playing song) it renders silence. Cells can be edited while playing.
