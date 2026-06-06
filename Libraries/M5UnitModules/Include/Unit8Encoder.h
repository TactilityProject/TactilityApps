/*
 * Unit8Encoder.h - M5Stack 8Encoder Unit (STM32, I2C addr 0x41)
 *
 * 8 rotary encoders with push buttons, individual RGB LEDs, plus one toggle
 * switch with its own RGB LED.
 *
 * Register map:
 *   0x00 + i*4  : int32_t  absolute counter for encoder i (0-3)  [R/W]
 *   0x10 + i*4  : int32_t  absolute counter for encoder i (4-7)  [R/W]
 *   0x20 + i*4  : int32_t  increment (delta) for encoder i (0-3), resets after read  [R]
 *   0x30 + i*4  : int32_t  increment (delta) for encoder i (4-7), resets after read  [R]
 *   0x40 + i    : uint8_t  write 1 to reset counter i to 0 (i = 0-7)  [W]
 *   0x50 + i    : uint8_t  button state for encoder i (0=released, 1=pressed)  [R]
 *   0x60        : uint8_t  toggle switch state (0=off, 1=on)  [R]
 *   0x70 + i*3  : [R, G, B]  LED colour for encoder i (i = 0-7, 0x00RRGGBB)  [R/W]
 *   0x88        : [R, G, B]  LED colour for the toggle switch (LED8)  [R/W]
 *   0xF0        : uint8_t  firmware version  [R]
 *   0xFF        : uint8_t  I2C address (R/W)
 *
 * LED indices: 0-7 = encoder LEDs, 8 = switch LED (total 9 LEDs).
 *
 * Usage:
 *   Unit8Encoder enc;
 *   if (enc.begin(dev)) { ... }        // dev = device_find_by_name("i2c1")
 *   enc.readAll(deltas, buttons);      // deltas in detents (÷4), buttons 0/1
 *   bool sw = false;
 *   if (enc.readSwitch(sw)) { sw holds switch state } // false = read failed
 *   enc.setAllLeds(0xFF0000);          // set all 9 LEDs (encoders + switch) to red
 *   enc.setSwitchLed(0x00FF00);        // set switch LED colour only
 */
#pragma once

#include <UnitCommon.h>
#include <cstdint>

class Unit8Encoder {
public:
    static constexpr uint8_t DEFAULT_ADDR = 0x41;
    static constexpr uint8_t LED_COUNT         = 9;  // 8 encoder LEDs + 1 switch LED
    static constexpr uint8_t ENCODER_LED_COUNT = 8;

    Unit8Encoder() = default;
    Unit8Encoder(const Unit8Encoder&) = delete;
    Unit8Encoder& operator=(const Unit8Encoder&) = delete;

    // Pass a I2C controller device
    // Probe and initialise. Returns false if the unit is not present.
    [[nodiscard]] bool begin(Device* dev, uint8_t addr = DEFAULT_ADDR);

    bool isPresent() const { return dev_ != nullptr; }

    // Read all 8 encoder deltas (in detents, hardware counts ÷4) and button states.
    // Returns false on I2C error.
    bool readAll(int32_t deltas[8], uint8_t buttons[8]);

    // Read the toggle switch state into `state`. Returns false on I2C error.
    // On error, `state` is left unchanged. Use the return value to distinguish
    // "switch off" (returns true, state=false) from read failure (returns false).
    bool readSwitch(bool& state);

    // Set one LED immediately (0x00RRGGBB). Updates cached colour.
    // idx 0-7 = encoder LEDs, idx 8 = switch LED. Silently ignores idx > 8.
    void setLed(uint8_t idx, uint32_t rgb);

    // Set the switch LED colour (0x00RRGGBB) immediately.
    void setSwitchLed(uint32_t rgb);

    // Flush pending[LED_COUNT] to hardware if anything changed.
    // pending[0-7] = encoder LEDs, pending[8] = switch LED.
    // Call every poll with desired colours; only sends if dirty.
    void flushLeds(const uint32_t pending[LED_COUNT]);

    // Set all 9 LEDs (encoder + switch) to one colour immediately.
    void setAllLeds(uint32_t rgb);

    // Cached LED colours (0x00RRGGBB). Read-only - updated by set*/flush*.
    // Index 0-7 = encoder LEDs, index 8 = switch LED.
    const uint32_t* ledColors() const { return ledColor_; }

private:
    Device*  dev_  = nullptr;
    uint8_t  addr_ = DEFAULT_ADDR;
    uint32_t ledColor_[LED_COUNT] = {};

    static constexpr uint8_t REG_INCREMENT  = 0x20;
    static constexpr uint8_t REG_BUTTON     = 0x50;
    static constexpr uint8_t REG_SWITCH     = 0x60;
    static constexpr uint8_t REG_LED        = 0x70;  // encoder LEDs 0-7 (24 bytes)
    static constexpr uint8_t REG_SWITCH_LED = 0x88;  // switch LED (3 bytes)
};
