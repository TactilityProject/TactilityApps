/*
 * UnitMidi.h - M5Stack MIDI Unit / Synth Unit (SAM2695, UART 31250 bps)
 *
 * Both the MIDI Unit and the Synth Unit use the SAM2695 audio synthesizer IC
 * connected via a standard MIDI UART link. This driver covers both.
 *
 * SAM2695 specs:
 *   - 64-voice polyphony (38 with effects)
 *   - 16 MIDI channels
 *   - MIDI 1.0 protocol, 31250 bps, 8N1
 *
 * Usage:
 *   Device* uart = device_find_by_name("uart1");
 *   UnitMidi midi;
 *   if (midi.begin(uart)) {
 *       midi.programChange(0, 0);     // channel 0, Grand Piano
 *       midi.noteOn(0, 60, 100);      // middle C, velocity 100
 *       vTaskDelay(pdMS_TO_TICKS(500));
 *       midi.noteOff(0, 60);
 *   }
 */
#pragma once

#include <tactility/device.h>
#include <cstddef>
#include <cstdint>

class UnitMidi {
public:
    UnitMidi() = default;
    ~UnitMidi() { end(); }
    UnitMidi(const UnitMidi&) = delete;
    UnitMidi& operator=(const UnitMidi&) = delete;
    UnitMidi(UnitMidi&&) = delete;
    UnitMidi& operator=(UnitMidi&&) = delete;

    // Pass a UART controller device
    // begin() opens the controller, sets baud rate to 31250, and issues a system reset.
    [[nodiscard]] bool begin(Device* dev);

    void end();

    bool isPresent() const { return ready_; }

    // Send raw MIDI bytes.
    void send(const uint8_t* data, size_t len);

    // MIDI 1.0 message helpers
    void reset();                                                 // 0xFF - system reset
    void noteOn(uint8_t channel, uint8_t note, uint8_t velocity); // 0x90|ch, note, vel
    void noteOff(uint8_t channel, uint8_t note);                  // 0x80|ch, note, 0
    void programChange(uint8_t channel, uint8_t program);         // 0xC0|ch, program
    void controlChange(uint8_t channel, uint8_t controller, uint8_t value); // 0xB0|ch, ctrl, val
    void pitchBend(uint8_t channel, int16_t value);               // 0xE0|ch, LSB, MSB (-8192..+8191)
    void allNotesOff(uint8_t channel);                            // CC 123 = 0

private:
    Device* dev_  = nullptr;
    bool ready_   = false;
};
