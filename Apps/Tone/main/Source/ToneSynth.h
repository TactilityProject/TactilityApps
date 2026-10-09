// SPDX-License-Identifier: GPL-3.0
#pragma once

#include <cstdint>

/** Waveforms the tone generator can produce, in sync with TONE_WAVEFORM_LABELS. */
enum class ToneWaveform : uint8_t {
    Sine,
    Square,
    Saw,
    Triangle,
    Noise,
};

namespace tone_synth {

constexpr uint32_t SAMPLE_RATE = 48000;
constexpr uint32_t MIN_FREQUENCY_HZ = 20;
constexpr uint32_t MAX_FREQUENCY_HZ = 20000;

/** Mutable per-run state of the sample generator. */
struct SynthState {
    float phase = 0.0f;
    uint32_t noiseSeed = 0x12345678u;
    uint32_t sampleIndex = 0;
    uint32_t sweepPeriod = 0;
    uint32_t sweepQ = 0;
    uint32_t sweepT = 0;
    uint32_t sweepOffset = 0;
    int32_t sweepAcc = 0;
    bool sweepRising = true;
};

/** Clamps a requested frequency into the audible range [MIN_FREQUENCY_HZ, MAX_FREQUENCY_HZ]. */
uint32_t clamp_frequency(uint32_t frequency);

/**
 * Frequency to produce for the current sample of a playback run.
 * A sweep produces a triangle (up-down) ramp between minHz and maxHz covering a
 * full cycle every periodSamples samples
 */
uint32_t frequency_at(uint32_t fixedHz, bool sweep, uint32_t minHz, uint32_t maxHz,
                      uint32_t periodSamples, SynthState& state);

/**
 * Produces the next monophonic sample for the given waveform at the given frequency,
 * advancing @a state. The returned value is in [-1, 1] regardless of waveform.
 */
float next_sample(ToneWaveform waveform, uint32_t frequency, SynthState& state);

}