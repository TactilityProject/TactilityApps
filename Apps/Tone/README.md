# Tone

A signal generator for Tactility devices. Plays oscillator waveforms straight to the audio codec with live control over frequency, waveform, and a configurable frequency sweep.

## Features

- **5 waveforms**: sine, square, sawtooth, triangle, and white noise
- **9 frequency presets**: 50 Hz - 10 kHz, or a free-entry frequency field
- **Frequency sweep**: triangle up-down sweep between a min and max frequency with an adjustable period
- **Mono and stereo output** (configurable before playback)
- **Live updates**: frequency, waveform, and sweep settings apply immediately while playing, via lock-free atomics
- **100% volume control** through the sliderbox
- **Adaptive layout** for Cardputer, T-Deck, CYD, and compact panels

## Controls

- **Play/Pause** (toolbar button): start or stop the tone. Channel selection locks while playing.
- **Frequency chips**: select a preset or the Sweep mode. Type into the frequency field for a custom value.
- **Sweep card**: shown only in sweep mode; edit min Hz, max Hz, and period (in seconds).
- **Mono/Stereo chips**: pick the output channel config (only while stopped).
- **Volume slider**: adjusts the codec output volume live.

## Notes

- Frequencies are clamped to the 20 Hz - 20 kHz audible range.
