#include "Bleep/Player.h"

namespace bleep {

Player::Player() = default;

void Player::lock() {
    // The audio task holds it only while rendering a chunk (well under a millisecond)
    while (busy.exchange(true, std::memory_order_acquire)) {
    }
}

void Player::play(const Song* song, int fromBar) {
    lock();
    sequencer.start(synth, song, fromBar);
    isPlaying.store(song != nullptr);
    position.store(-1);
    unlock();
}

void Player::stop() {
    play(nullptr);
}

void Player::setLoop(int first, int last) {
    lock();
    sequencer.setLoop(first, last);
    unlock();
}

void Player::setMuted(uint8_t mask) {
    lock();
    sequencer.setMuted(mask);
    unlock();
}

void Player::audition(Channel channel, uint8_t cell, int bpm) {
    if (cell == CellRest || cell == CellHold) {
        return;
    }
    lock();
    Sequencer::trigger(synth, AUDITION_VOICE, channel, cell, SAMPLE_RATE * 60 / (bpm * 4));
    unlock();
}

void Player::setVolume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    gain.store(12 * 256 * percent / 100);
}

void Player::render(int16_t* out, int frames) {
    if (busy.exchange(true, std::memory_order_acquire)) {
        for (int i = 0; i < frames; i++) out[i] = 0; // being changed: never wait in the audio task
        return;
    }
    const int masterGain = gain.load();
    for (int i = 0; i < frames; i++) {
        sequencer.tick(synth);
        int mix = synth.mix() * masterGain >> 8;
        if (mix > 32767) mix = 32767;
        if (mix < -32768) mix = -32768;
        out[i] = (int16_t)mix;
    }
    if (sequencer.song() != nullptr && sequencer.bar() >= 0) {
        position.store(sequencer.bar() << 8 | sequencer.row());
    }
    busy.store(false, std::memory_order_release);
}

} // namespace bleep
