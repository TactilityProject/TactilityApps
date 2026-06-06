/*
 * UnitByteButton.h - M5Stack ByteButton Unit (STM32, I2C addr 0x47)
 *
 * 8 momentary buttons with individual RGB LEDs.
 *
 * Register map:
 *   0x00        : uint8_t  all 8 button states as a bitmask (bit i = button i)
 *   0x10 + i    : uint8_t  LED brightness for button i (0-255)
 *   0x19        : uint8_t  LED show mode (0=user-defined, 1=system-default)
 *   0x20 + i*4  : uint32_t LED RGB colour for button i (LE: bytes = [B,G,R,0x00])
 *   0x50 + i    : uint8_t  LED colour in RGB233 compressed format
 *   0x60 + i    : uint8_t  individual button state (0=released, non-zero=pressed)
 *   0x70 + i*4  : uint32_t system "off" colour for button i
 *   0x90 + i*4  : uint32_t system "on"  colour for button i
 *   0xFE        : uint8_t  firmware version
 *   0xFF        : uint8_t  I2C address (R/W)
 *
 * LED colour format: write uint32_t 0x00RRGGBB as little-endian → wire sees [BB, GG, RR, 00]
 *
 * Usage:
 *   UnitByteButton bb;
 *   if (bb.begin(dev)) { ... }
 *   uint8_t mask = bb.readButtons();   // bitmask, bit 0 = button 0
 *   bb.setLed(i, 0xFF4400);            // per-button
 *   bb.flushLeds(pending);             // burst write if dirty
 */
#pragma once

#include <UnitCommon.h>
#include <cstdint>

class UnitByteButton {
public:
    static constexpr uint8_t DEFAULT_ADDR  = 0x47;
    static constexpr uint8_t BUTTON_COUNT  = 8;

    UnitByteButton() = default;
    UnitByteButton(const UnitByteButton&) = delete;
    UnitByteButton& operator=(const UnitByteButton&) = delete;

    // Pass a I2C controller device
    // Probe and initialise. Returns false if not present.
    // Sets show mode to user-defined so setLed/flushLeds take effect.
    [[nodiscard]] bool begin(Device* dev, uint8_t addr = DEFAULT_ADDR);

    bool isPresent() const { return dev_ != nullptr; }

    // Read all 8 buttons as a bitmask. Bit i = button i (1=pressed).
    // Returns the bitmask via value; sets *ok=false on I2C error (ok may be nullptr).
    uint8_t readButtons(bool* ok = nullptr);

    // Read individual button state (idx 0-7). Returns true if pressed.
    bool readButton(uint8_t idx);

    // Set one LED (0x00RRGGBB, idx 0-7). Updates cache and sends immediately.
    void setLed(uint8_t idx, uint32_t rgb);

    // Flush pending[BUTTON_COUNT] to hardware in one burst if anything changed
    // since the last flush. pending[] entries are 0x00RRGGBB.
    void flushLeds(const uint32_t pending[BUTTON_COUNT]);

    // Set all 8 LEDs to one colour immediately.
    void setAllLeds(uint32_t rgb);

    const uint32_t* ledColors() const { return ledColor_; }

private:
    Device*  dev_  = nullptr;
    uint8_t  addr_ = DEFAULT_ADDR;
    uint32_t ledColor_[BUTTON_COUNT] = {};

    static constexpr uint8_t REG_STATUS    = 0x00;
    static constexpr uint8_t REG_SHOW_MODE = 0x19;
    static constexpr uint8_t REG_RGB888    = 0x20;  // + i*4, LE uint32_t
    static constexpr uint8_t REG_STATUS_8  = 0x60;  // + i, individual

};
