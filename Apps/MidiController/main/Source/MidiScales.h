#pragma once

#include <cstdint>
#include <cstdio>

// ---------------------------------------------------------------------------
// Scale definitions
// ---------------------------------------------------------------------------

struct MidiScale {
    const char* name;
    uint8_t intervals[12];
    int numNotes;
};

static constexpr MidiScale MIDI_SCALES[] = {
    { "Major",  { 0, 2, 4, 5, 7, 9, 11 }, 7 },
    { "Minor",  { 0, 2, 3, 5, 7, 8, 10 }, 7 },
    { "Dorian", { 0, 2, 3, 5, 7, 9, 10 }, 7 },
    { "Penta",  { 0, 2, 4, 7, 9 },         5 },
    { "Blues",  { 0, 3, 5, 6, 7, 10 },     6 },
    { "Chrome", { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }, 12 },
};
static constexpr int NUM_MIDI_SCALES = sizeof(MIDI_SCALES) / sizeof(MIDI_SCALES[0]);

// Returns the MIDI note number for a given scale degree, octave, and key transpose.
// degree can exceed numNotes - it wraps and increments the octave.
inline int getNoteInScale(int scaleIdx, int degree, int octave, int keyTranspose = 0) {
    scaleIdx = ((scaleIdx % NUM_MIDI_SCALES) + NUM_MIDI_SCALES) % NUM_MIDI_SCALES;
    const MidiScale& s = MIDI_SCALES[scaleIdx];
    int actualDegree = ((degree % s.numNotes) + s.numNotes) % s.numNotes;
    int octaveOffset = (degree >= 0) ? (degree / s.numNotes) : ((degree - s.numNotes + 1) / s.numNotes);
    int note = 60 + s.intervals[actualDegree] + ((octave - 4 + octaveOffset) * 12) + keyTranspose;
    // Clamp to valid MIDI range
    if (note < 0)   note = 0;
    if (note > 127) note = 127;
    return note;
}

// Returns true if a given MIDI pitch class (0-11) is in the specified scale + key.
inline bool isNoteInScale(int scaleIdx, int keyTranspose, int midiNote) {
    scaleIdx = ((scaleIdx % NUM_MIDI_SCALES) + NUM_MIDI_SCALES) % NUM_MIDI_SCALES;
    const MidiScale& s = MIDI_SCALES[scaleIdx];
    int pitchClass = ((midiNote - keyTranspose) % 12 + 12) % 12;
    for (int i = 0; i < s.numNotes; i++) {
        if (s.intervals[i] == pitchClass) return true;
    }
    return false;
}

// Returns true if the pitch class is the root note.
inline bool isRootNote(int keyTranspose, int midiNote) {
    return (((midiNote - keyTranspose) % 12 + 12) % 12) == 0;
}

// Fills buf with a sharp-based note name like "C4", "C#4", "A#3", etc.
inline const char* midiNoteName(int note, char* buf, int bufLen) {
    static const char* NAMES[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    if (note < 0) note = 0;
    int octave = (note / 12) - 1;  // MIDI 60 = C4 (scientific pitch notation)
    snprintf(buf, bufLen, "%s%d", NAMES[note % 12], octave);
    return buf;
}

// Returns short note name without octave (e.g. "C#")
inline const char* midiNoteClass(int note, char* buf, int bufLen) {
    static const char* NAMES[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    if (note < 0) note = 0;
    snprintf(buf, bufLen, "%s", NAMES[note % 12]);
    return buf;
}

// Key names for root note display (C, C#, D, ...)
static constexpr const char* KEY_NAMES[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};
