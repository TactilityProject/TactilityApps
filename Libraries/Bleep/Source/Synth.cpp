#include "Bleep/Synth.h"

namespace bleep {

namespace {

// Frequencies of MIDI notes 0..127, built by repeated semitone steps (no libm needed)
float noteFrequencies[128];
bool noteTableBuilt = false;

void buildNoteTable() {
    if (noteTableBuilt) return;
    const float semitone = 1.0594630943592953f;
    noteFrequencies[69] = 440.0f;
    for (int n = 70; n < 128; n++) noteFrequencies[n] = noteFrequencies[n - 1] * semitone;
    for (int n = 68; n >= 0; n--) noteFrequencies[n] = noteFrequencies[n + 1] / semitone;
    noteTableBuilt = true;
}

} // namespace

Synth::Synth() {
    buildNoteTable();
}

uint32_t Synth::frequencyToStep(float hz) {
    return (uint32_t)(hz * (4294967296.0f / (float)SAMPLE_RATE));
}

float Synth::noteFrequency(int midiNote) {
    buildNoteTable();
    return noteFrequencies[midiNote & 127];
}

void Synth::note(int voice, Wave wave, int midiNote, int volume, int lengthSamples, int decaySamples) {
    Voice& v = voices[voice];
    v.wave = wave;
    v.active = true;
    v.step = frequencyToStep(noteFrequencies[midiNote & 127]);
    v.stepSlide = 0;
    v.volume = volume << VOLUME_SHIFT;
    v.volumeSlide = decaySamples > 0 ? -((volume << VOLUME_SHIFT) / decaySamples + 1) : 0;
    v.length = lengthSamples;
}

void Synth::sweep(int voice, Wave wave, float fromHz, float toHz, int ms, int volume) {
    Voice& v = voices[voice];
    const int length = msToSamples(ms) > 0 ? msToSamples(ms) : 1;
    v.wave = wave;
    v.active = true;
    v.step = frequencyToStep(fromHz);
    v.stepSlide = (int32_t)(((int64_t)frequencyToStep(toHz) - (int64_t)v.step) / length);
    v.volume = volume << VOLUME_SHIFT;
    v.volumeSlide = -((volume << VOLUME_SHIFT) / length + 1);
    v.length = length;
}

void Synth::tone(int voice, Wave wave, float hz, int volume) {
    Voice& v = voices[voice];
    v.wave = wave;
    v.active = volume > 0;
    v.step = frequencyToStep(hz);
    v.stepSlide = 0;
    v.volume = volume << VOLUME_SHIFT;
    v.volumeSlide = 0;
    v.length = 1 << 30;
}

void Synth::stop(int voice) {
    voices[voice].active = false;
}

void Synth::stopAll() {
    for (auto& v : voices) v.active = false;
}

int Synth::sample(Voice& v) {
    const uint32_t previous = v.phase;
    v.phase += v.step;
    v.step = (uint32_t)((int64_t)v.step + v.stepSlide);
    int value;
    switch (v.wave) {
        case Wave::Pulse12: value = v.phase < 0x20000000u ? 1024 : -1024; break;
        case Wave::Pulse25: value = v.phase < 0x40000000u ? 1024 : -1024; break;
        case Wave::Pulse50: value = v.phase < 0x80000000u ? 1024 : -1024; break;
        case Wave::Triangle: {
            const int p = (int)(v.phase >> 21); // 0..2047
            value = p < 1024 ? p * 2 - 1024 : 3071 - p * 2;
            break;
        }
        case Wave::Saw: value = (int)(v.phase >> 21) - 1024; break;
        case Wave::Noise:
        default:
            if (v.phase < previous) { // a new random value once per cycle: the step sets the pitch
                v.noise ^= v.noise << 13;
                v.noise ^= v.noise >> 17;
                v.noise ^= v.noise << 5;
                v.noiseValue = (int16_t)((int)(v.noise & 2047) - 1024);
            }
            value = v.noiseValue;
            break;
    }
    const int out = value * (v.volume >> VOLUME_SHIFT) >> 12;
    v.volume += v.volumeSlide;
    if (--v.length <= 0 || v.volume <= 0) {
        v.active = false;
    }
    return out;
}

int Synth::mix() {
    int sum = 0;
    for (auto& v : voices) {
        if (v.active) sum += sample(v);
    }
    return sum;
}

} // namespace bleep
