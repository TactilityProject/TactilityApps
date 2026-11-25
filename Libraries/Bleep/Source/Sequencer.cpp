#include "Bleep/Sequencer.h"

namespace bleep {

namespace {

const Wave WAVES[3] = { Wave::Pulse25, Wave::Pulse12, Wave::Triangle };
const int VOLUMES[3] = { 1100, 420, 1500 };

} // namespace

void Sequencer::trigger(Synth& synth, int voice, Channel channel, uint8_t cell, int rowLength) {
    if (cell == CellRest) {
        if (channel != Drums) synth.stop(voice); // drum hits ring out on their own
        return;
    }
    if (cell == CellHold) {
        return;
    }
    if (channel == Drums) {
        switch (cell) {
            case CellKick: synth.sweep(voice, Wave::Triangle, 150.0f, 15.6f, 110, 2600); break;
            case CellSnare: synth.sweep(voice, Wave::Noise, 4000.0f, 4000.0f, 120, 1500); break;
            case CellHat: synth.sweep(voice, Wave::Noise, 9000.0f, 9000.0f, 35, 500); break;
            default: break;
        }
        return;
    }
    if (cell < CellFirstNote || cell > CellLastNote) {
        return;
    }
    // Plucky: fades over a few rows; the thin harmony fades within one
    synth.note(voice, WAVES[channel], cell, VOLUMES[channel], rowLength * 8, channel == Harmony ? rowLength : rowLength * 6);
}

void Sequencer::start(Synth& synth, const Song* song, int bar) {
    current = song;
    nextBar = bar;
    nextRow = 0;
    playedBar = -1;
    playedRow = -1;
    samplesToNextRow = 0;
    for (int c = 0; c < CHANNEL_COUNT; c++) {
        synth.stop(firstVoice + c);
    }
}

void Sequencer::setLoop(int first, int last) {
    loopFirst = first;
    loopLast = last;
}

void Sequencer::tick(Synth& synth) {
    if (current == nullptr) {
        return;
    }
    if (--samplesToNextRow <= 0) {
        playRow(synth);
    }
}

void Sequencer::playRow(Synth& synth) {
    const Song& song = *current;
    const int barCount = (int)song.bars.size();
    if (barCount == 0) {
        samplesToNextRow = song.rowLength();
        return;
    }
    const bool looping = loopFirst >= 0 && loopLast >= loopFirst && loopFirst < barCount;
    const int first = looping ? loopFirst : 0;
    const int last = looping ? (loopLast < barCount ? loopLast : barCount - 1) : barCount - 1;
    if (nextBar < first || nextBar > last) {
        nextBar = first;
        nextRow = 0;
    }

    const Bar& bar = song.bars[nextBar];
    const int rowLength = song.rowLength();
    for (int c = 0; c < CHANNEL_COUNT; c++) {
        if (muted & (1 << c)) {
            synth.stop(firstVoice + c);
        } else {
            trigger(synth, firstVoice + c, (Channel)c, bar.cells[c][nextRow], rowLength);
        }
    }
    playedBar = nextBar;
    playedRow = nextRow;
    if (++nextRow >= ROWS_PER_BAR) {
        nextRow = 0;
        nextBar = nextBar >= last ? first : nextBar + 1;
    }
    samplesToNextRow = rowLength;
}

} // namespace bleep
