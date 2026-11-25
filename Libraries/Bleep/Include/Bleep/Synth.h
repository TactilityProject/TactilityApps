#pragma once

#include "Song.h"

#include <cstdint>

namespace bleep {

enum class Wave : uint8_t { Pulse12, Pulse25, Pulse50, Triangle, Saw, Noise };

/**
 * A small chiptune synthesiser: up to 8 voices of simple waves with a pitch slide and a linear
 * volume fade, mixed to mono. Integer only, so it's cheap on chips without an FPU.
 *
 * Not thread safe: use it from one thread (normally the audio task).
 */
class Synth {
public:
    static constexpr int MAX_VOICES = 8;
    static constexpr int MAX_VOLUME = 4096;

    Synth();

    /**
     * Starts a MIDI note on a voice.
     * @param volume 0..MAX_VOLUME
     * @param lengthSamples how long it plays at most
     * @param decaySamples how long it takes to fade out (0: no fade)
     */
    void note(int voice, Wave wave, int midiNote, int volume, int lengthSamples, int decaySamples);
    /** Starts a pitch sweep that fades out over its length (sound effects). */
    void sweep(int voice, Wave wave, float fromHz, float toHz, int ms, int volume);
    /** A steady tone until changed or stopped (e.g. an engine hum). */
    void tone(int voice, Wave wave, float hz, int volume);
    void stop(int voice);
    void stopAll();
    bool active(int voice) const { return voices[voice].active; }

    /** @return the next sample of all voices together, about +-1024 per voice at full volume */
    int mix();

    static uint32_t frequencyToStep(float hz);
    static float noteFrequency(int midiNote);
    static int msToSamples(int ms) { return ms * SAMPLE_RATE / 1000; }

private:
    // Volumes are kept with extra fractional bits, so long, slow fades don't round to nothing
    static constexpr int VOLUME_SHIFT = 8;

    struct Voice {
        Wave wave = Wave::Pulse50;
        bool active = false;
        uint32_t phase = 0;
        uint32_t step = 0; // phase increment per sample (32-bit fixed point of one cycle)
        int32_t stepSlide = 0; // added to step every sample (pitch sweep)
        int volume = 0; // with VOLUME_SHIFT fractional bits
        int volumeSlide = 0; // added every sample (fade)
        int length = 0; // samples left
        uint32_t noise = 0x12345u;
        int16_t noiseValue = 0;
    };

    Voice voices[MAX_VOICES];

    int sample(Voice& voice);
};

} // namespace bleep
