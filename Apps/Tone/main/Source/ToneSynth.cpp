// SPDX-License-Identifier: GPL-3.0
#include "ToneSynth.h"

#include <algorithm>
#include <cmath>

namespace tone_synth {

namespace {

constexpr float TWO_PI = 6.28318530717958647692f;
constexpr float INVERSE_TWO_PI = 0.15915494309189535f;
constexpr float INVERSE_SAMPLE_RATE = 1.0f / SAMPLE_RATE;

} // namespace

uint32_t clamp_frequency(uint32_t frequency) {
    return std::max<uint32_t>(MIN_FREQUENCY_HZ, std::min<uint32_t>(frequency, MAX_FREQUENCY_HZ));
}

uint32_t frequency_at(uint32_t fixedHz, bool sweep, uint32_t minHz, uint32_t maxHz,
                      uint32_t periodSamples, SynthState& state) {
    if (!sweep || periodSamples == 0) {
        return clamp_frequency(fixedHz);
    }

    const uint32_t low = clamp_frequency(std::min(minHz, maxHz));
    const uint32_t high = clamp_frequency(std::max(minHz, maxHz));
    const uint32_t q = high - low;
    const uint32_t riseLen = periodSamples / 2;
    if (q == 0 || riseLen == 0) {
        return low;
    }

    if (state.sweepPeriod != periodSamples || state.sweepQ != q) {
        // Period or range changed since the last sample: restart the sweep cycle.
        state.sweepPeriod = periodSamples;
        state.sweepQ = q;
        state.sweepT = 0;
        state.sweepOffset = 0;
        state.sweepAcc = 0;
        state.sweepRising = true;
    }

    const uint32_t result = low + state.sweepOffset;
    const uint32_t step = q / riseLen;
    const uint32_t rem = q % riseLen;
    state.sweepT++;
    if (state.sweepT >= periodSamples) {
        state.sweepT = 0;
        state.sweepOffset = 0;
        state.sweepAcc = 0;
        state.sweepRising = true;
    } else if (state.sweepRising) {
        if (state.sweepT == riseLen) {
            state.sweepRising = false;
            state.sweepOffset = q;
            state.sweepAcc = 0;
        } else {
            state.sweepAcc += static_cast<int32_t>(rem);
            state.sweepOffset += step;
            if (state.sweepAcc >= static_cast<int32_t>(riseLen)) {
                state.sweepAcc -= static_cast<int32_t>(riseLen);
                state.sweepOffset += 1;
            }
        }
    } else {
        state.sweepAcc -= static_cast<int32_t>(rem);
        state.sweepOffset -= step;
        if (state.sweepAcc < 0) {
            state.sweepAcc += static_cast<int32_t>(riseLen);
            state.sweepOffset -= 1;
        }
    }
    return result;
}

float next_sample(ToneWaveform waveform, uint32_t frequency, SynthState& state) {
    const float phaseFraction = state.phase * INVERSE_TWO_PI;
    float sample = 0.0f;
    switch (waveform) {
        case ToneWaveform::Sine:
            sample = std::sin(state.phase);
            break;
        case ToneWaveform::Square:
            sample = phaseFraction < 0.5f ? 1.0f : -1.0f;
            break;
        case ToneWaveform::Saw:
            sample = 2.0f * phaseFraction - 1.0f;
            break;
        case ToneWaveform::Triangle:
            if (phaseFraction < 0.25f) {
                sample = 4.0f * phaseFraction;
            } else if (phaseFraction < 0.75f) {
                sample = 2.0f - 4.0f * phaseFraction;
            } else {
                sample = 4.0f * phaseFraction - 4.0f;
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

    state.phase += TWO_PI * static_cast<float>(frequency) * INVERSE_SAMPLE_RATE;
    if (state.phase >= TWO_PI) {
        state.phase -= TWO_PI;
    }
    state.sampleIndex++;
    return sample;
}

} // namespace tone_synth