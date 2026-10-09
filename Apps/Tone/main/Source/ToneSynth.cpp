// SPDX-License-Identifier: GPL-3.0
#include "ToneSynth.h"

#include <algorithm>
#include <cmath>

namespace tone_synth {

namespace {

constexpr double TWO_PI = 6.28318530717958647692;

} // namespace

uint32_t clamp_frequency(uint32_t frequency) {
    return std::max<uint32_t>(MIN_FREQUENCY_HZ, std::min<uint32_t>(frequency, MAX_FREQUENCY_HZ));
}

uint32_t frequency_at(uint32_t sampleIndex, uint32_t fixedHz, bool sweep,
                      uint32_t minHz, uint32_t maxHz, uint32_t periodSamples) {
    if (!sweep || periodSamples == 0) {
        return clamp_frequency(fixedHz);
    }

    const uint32_t low = clamp_frequency(std::min(minHz, maxHz));
    const uint32_t high = clamp_frequency(std::max(minHz, maxHz));
    if (low == high) {
        return low;
    }

    const uint32_t halfPeriod = periodSamples / 2;
    if (halfPeriod == 0) {
        return low;
    }

    const uint32_t t = sampleIndex % periodSamples;
    // Triangle sweep: rise through the first half of the period, fall through the second.
    const double normalized = t < halfPeriod
        ? static_cast<double>(t) / halfPeriod
        : 2.0 - static_cast<double>(t) / halfPeriod;
    return low + static_cast<uint32_t>(static_cast<double>(high - low) * normalized + 0.5);
}

float next_sample(ToneWaveform waveform, uint32_t frequency, SynthState& state) {
    const double phaseFraction = state.phase / TWO_PI;
    float sample = 0.0f;
    switch (waveform) {
        case ToneWaveform::Sine:
            sample = static_cast<float>(std::sin(state.phase));
            break;
        case ToneWaveform::Square:
            sample = phaseFraction < 0.5 ? 1.0f : -1.0f;
            break;
        case ToneWaveform::Saw:
            sample = static_cast<float>(2.0 * phaseFraction - 1.0);
            break;
        case ToneWaveform::Triangle:
            if (phaseFraction < 0.25) {
                sample = static_cast<float>(4.0 * phaseFraction);
            } else if (phaseFraction < 0.75) {
                sample = static_cast<float>(2.0 - 4.0 * phaseFraction);
            } else {
                sample = static_cast<float>(4.0 * phaseFraction - 4.0);
            }
            break;
        case ToneWaveform::Noise: {
            uint32_t x = state.noiseSeed;
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            state.noiseSeed = x;
            // Top 16 bits scaled to [-1, 1).
            sample = static_cast<float>(static_cast<int32_t>(x >> 16)) / 32768.0f - 1.0f;
            break;
        }
    }

    state.phase += TWO_PI * frequency / SAMPLE_RATE;
    if (state.phase >= TWO_PI) {
        state.phase -= TWO_PI;
    }
    state.sampleIndex++;
    return sample;
}

} // namespace tone_synth