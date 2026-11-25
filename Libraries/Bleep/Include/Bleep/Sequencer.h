#pragma once

#include "Song.h"
#include "Synth.h"

namespace bleep {

/**
 * Plays a Song on four voices of a Synth (firstVoice .. firstVoice + 3): lead, harmony, bass and
 * drums. Call tick() once per sample, from the thread that owns the synth.
 *
 * The song is read while playing, so it must stay alive, and changing it (cells, bars or bpm)
 * must not race with tick() (see Player for a way to do that).
 */
class Sequencer {
public:
    explicit Sequencer(int firstVoice = 0) : firstVoice(firstVoice) {}

    /** Plays a song (nullptr: stop) from the start of a bar. */
    void start(Synth& synth, const Song* song, int bar = 0);
    void stop(Synth& synth) { start(synth, nullptr); }
    /** Loops bars first..last (inclusive) instead of the whole song. -1: the whole song. */
    void setLoop(int first, int last);
    /** Channels to keep silent, as bits (1 << Channel) */
    void setMuted(uint8_t mask) { muted = mask; }
    uint8_t mutedMask() const { return muted; }

    void tick(Synth& synth);

    const Song* song() const { return current; }
    /** The bar and row last played (-1 before the first row) */
    int bar() const { return playedBar; }
    int row() const { return playedRow; }

    /** Plays one cell the way a channel does, on any voice (e.g. to audition a note). */
    static void trigger(Synth& synth, int voice, Channel channel, uint8_t cell, int rowLength);

private:
    int firstVoice;
    const Song* current = nullptr;
    int nextBar = 0;
    int nextRow = 0;
    int playedBar = -1;
    int playedRow = -1;
    int samplesToNextRow = 0;
    int loopFirst = -1;
    int loopLast = -1;
    uint8_t muted = 0;

    void playRow(Synth& synth);
};

} // namespace bleep
