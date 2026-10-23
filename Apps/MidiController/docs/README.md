# MidiController — Documentation

MIDI controller app for the M5Stack Tab5 (ESP32-P4) built on the Tactility SDK.
Sends standard 3-byte MIDI over Bluetooth LE MIDI or USB MIDI to any DAW, synth, or hardware.

## Contents

| File | What it covers |
|------|---------------|
| [hardware.md](hardware.md) | Tab5 device, Port A wiring, I2C units, PaHub channel map |
| [architecture.md](architecture.md) | Code layout, mode lifecycle, LVGL patterns, BLE/USB MIDI plumbing |
| [modes.md](modes.md) | All 15 modes — parameters, controls, musical function |
| [encoder.md](encoder.md) | 8Encoder knob/button/LED assignments per mode |
| [lcd.md](lcd.md) | LCD Unit display layouts per mode |
| [scales.md](scales.md) | Scale and key system, MIDI note helpers |
| [units.md](units.md) | Unit drivers in `Libraries/M5UnitModules` (8Encoder, PaHub, LCD, ByteButton, Joystick2, Scroll) |

## Quick start

1. From the repository root, build and install with `.\tt.ps1 app bir --path Apps/MidiController` (or run `.\tt.ps1 app build --path Apps/MidiController` followed by `.\tt.ps1 app install --path Apps/MidiController`).
2. Pick **BLUETOOTH LE** or **USB** on the transport screen shown at launch (unless BLE MIDI is already running). You can change it later with the `USB`/`BLE` button in the toolbar while sending is stopped.
3. Flip the toolbar switch to start sending. For BLE, pair with your DAW/synth — the device advertises as **TT MIDI**.
4. Tap any mode tile on the home screen to begin.

Hardware accessories (8Encoder, LCD Unit) are optional — the app works without them.
