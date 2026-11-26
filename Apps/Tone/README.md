# Tone

A signal generator for Tactility devices. Plays oscillator waveforms straight to the audio codec with live control over frequency, waveform, and a configurable frequency sweep.

## Features

- **5 waveforms**: sine, square, sawtooth, triangle, and noise (darker at low frequencies, close to white noise at high ones), with a preview
- **Frequency**: a logarithmic slider, fine steps, presets from 50 Hz to 10 kHz, or type an exact value
- **Note names**: the nearest note and the offset in cents
- **Frequency sweep**: up and down between two frequencies with an adjustable period
- **Live updates**: all settings apply while playing
- **Layouts** for small screens and large landscape or portrait screens

## Controls

- **Play / Stop**: start or stop the tone
- **− / +**: steps of 1 Hz below 100 Hz, 10 Hz below 1 kHz, 100 Hz above
- **Tap the frequency**: type an exact value
- **Menu** (small screens): presets, sweep settings and typing a frequency
- **Sweep**: drag the sliders, or tap a value to type it

When a screen has no room, the presets are left out first (they stay in the menu), then the wave preview.

## Notes

- Frequencies are clamped to the 20 Hz - 20 kHz audible range.
