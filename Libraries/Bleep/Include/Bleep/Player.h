#pragma once

#include "AudioSource.h"
#include "Sequencer.h"
#include "Song.h"
#include "Synth.h"

#include <atomic>

namespace bleep {

/**
 * Plays songs and single notes, for an app that only needs music. Controlled from one thread
 * (e.g. the UI), rendered from the audio task (see AudioOutput).
 *
 * The audio task never waits: while the controlling thread holds the lock, it renders silence.
 */
class Player : public AudioSource {
public:
    Player();

    /**
     * Plays a song in a loop, from the start of a bar. The song must stay alive until stop() or
     * the next play(). Edit its cells freely; change its bars (or bpm) only between lock() and
     * unlock().
     */
    void play(const Song* song, int fromBar = 0);
    void stop();
    bool playing() const { return isPlaying.load(); }
    /** Loops bars first..last (inclusive). -1: the whole song. */
    void setLoop(int first, int last);
    /** Channels to keep silent, as bits (1 << Channel) */
    void setMuted(uint8_t mask);
    /** Plays one cell as its channel would (on a voice of its own), e.g. when it's entered. */
    void audition(Channel channel, uint8_t cell, int bpm);

    /** The bar and row playing now (-1 when stopped) */
    int bar() const { return position.load() >> 8; }
    int row() const { return position.load() & 0xFF; }

    /** Master volume, 0..100 */
    void setVolume(int percent);

    void lock();
    void unlock() { busy.store(false, std::memory_order_release); }

    void render(int16_t* out, int frames) override;

private:
    static constexpr int AUDITION_VOICE = CHANNEL_COUNT;

    Synth synth;
    Sequencer sequencer { 0 };
    std::atomic<bool> busy { false };
    std::atomic<bool> isPlaying { false };
    std::atomic<int> position { -1 };
    std::atomic<int> gain { 12 * 256 };
};

} // namespace bleep
