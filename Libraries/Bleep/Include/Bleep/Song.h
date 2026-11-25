#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bleep {

constexpr int SAMPLE_RATE = 22050;
constexpr int ROWS_PER_BAR = 16; // a bar is 16 rows of 16th notes

/** The four channels of a song. */
enum Channel : uint8_t { Lead, Harmony, Bass, Drums, CHANNEL_COUNT };

/**
 * One row of one channel. Melodic channels hold a MIDI note (C-0 = 12 up to B-8 = 119), drums a
 * drum. Values below 12 are special.
 */
enum Cell : uint8_t {
    CellRest = 0, // "..." silence (stops the note)
    CellHold = 1, // "---" the previous note rings on
    CellKick = 2, // "K.." drums only
    CellSnare = 3, // "S.."
    CellHat = 4, // "H.."
    CellFirstNote = 12,
    CellLastNote = 119,
};

struct Bar {
    uint8_t cells[CHANNEL_COUNT][ROWS_PER_BAR] = {}; // all rests
};

/**
 * A chiptune: bars of four channels, played in order and looped.
 *
 * The text format (.bleep):
 *
 *   # comment              '#' at the start of a line, or followed by a space
 *   bpm 120                40-300
 *   bar                    starts a bar
 *   lead    F#5 --- ...    16 tokens per channel: a note (name, '#' or '-', octave 0-8), "---"
 *   harmony D-4 A-4 ...    to hold the previous note, "..." for silence
 *   bass    D-3 --- ...
 *   drums   K.. ... H..    K.. kick, S.. snare, H.. hi-hat, ... nothing
 *
 * Lead is a 25% pulse, harmony a thin 12.5% pulse, bass a triangle, drums noise and a pitched kick.
 * Channels a bar leaves out are silent.
 */
struct Song {
    int bpm = 120;
    std::vector<Bar> bars;

    /** Samples per row at SAMPLE_RATE */
    int rowLength() const { return SAMPLE_RATE * 60 / (bpm * 4); }
    /** Length of one loop of the song in milliseconds */
    int durationMs() const { return (int)((int64_t)bars.size() * ROWS_PER_BAR * 15000 / bpm); }
};

constexpr int MIN_BPM = 40;
constexpr int MAX_BPM = 300;

/** Parses .bleep text. @return false with "line N: message" in error when it's invalid */
bool parseSong(const std::string& text, Song& out, std::string& error);

/** Writes .bleep text, with an optional comment (one or more lines) at the top. */
std::string formatSong(const Song& song, const std::string& comment = std::string());

/** The 3 character text of a cell ("F#5", "---", "...", "K.."). @param out at least 4 chars */
void cellText(Channel channel, uint8_t cell, char* out);

/** "lead", "harmony", "bass", "drums" */
const char* channelName(Channel channel);

/** @return whether a cell value is valid in a channel */
bool cellValid(Channel channel, uint8_t cell);

} // namespace bleep
