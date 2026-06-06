/*
 * UnitScroll.h - M5Stack Scroll Unit (STM32, I2C addr 0x40)
 *
 * Rotary encoder with push button and one RGB LED.
 *
 * Register map:
 *   0x10        : int16_t   absolute encoder value
 *   0x20        : uint8_t   button (0=pressed, 1=released - inverted!)
 *   0x30        : 4 bytes   LED: [index(0), R, G, B]  - index byte always 0
 *   0x40        : uint8_t   write 1 to reset encoder to 0
 *   0x50        : int16_t   increment (delta) since last read - resets after read
 *   0xFE        : uint8_t   firmware version
 *   0xFF        : uint8_t   I2C address (R/W)
 *
 * Usage:
 *   UnitScroll scroll;
 *   if (scroll.begin(dev)) { ... }
 *   int16_t delta = scroll.readDelta();   // ±detents since last call
 *   bool btn = scroll.isPressed();
 *   scroll.setLed(0x00FF00);
 */
#pragma once

#include <UnitCommon.h>
#include <cstdint>

class UnitScroll {
public:
    static constexpr uint8_t DEFAULT_ADDR = 0x40;

    UnitScroll() = default;
    UnitScroll(const UnitScroll&) = delete;
    UnitScroll& operator=(const UnitScroll&) = delete;

    // Pass a I2C controller device
    [[nodiscard]] bool begin(Device* dev, uint8_t addr = DEFAULT_ADDR);
    bool isPresent() const { return dev_ != nullptr; }

    // Read increment (delta) since last call. Resets hardware counter.
    int16_t readDelta();

    // Read absolute encoder value.
    int16_t readAbsolute() const;

    // Button state. True = pressed (register value 0 = pressed, inverted hardware).
    bool isPressed() const;

    // Set the RGB LED colour (0x00RRGGBB).
    void setLed(uint32_t rgb);

    // Reset absolute encoder to 0.
    void resetEncoder();

private:
    Device*  dev_  = nullptr;
    uint8_t  addr_ = DEFAULT_ADDR;

    static constexpr uint8_t REG_ENCODER     = 0x10;
    static constexpr uint8_t REG_BUTTON      = 0x20;
    static constexpr uint8_t REG_LED         = 0x30;
    static constexpr uint8_t REG_RESET       = 0x40;
    static constexpr uint8_t REG_INC_ENCODER = 0x50;
};
