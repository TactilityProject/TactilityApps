/*
 * UnitJoystick2.h - M5Stack Joystick2 Unit (STM32, I2C addr 0x63)
 *
 * XY analogue joystick with push button and one RGB LED.
 *
 * Register map:
 *   0x00        : uint16_t[2] X, Y raw 12-bit ADC (4 bytes LE, 0-4095)
 *   0x10        : uint8_t[2]  X, Y 8-bit ADC (2 bytes)
 *   0x20        : uint8_t     button (0=pressed, 1=released - inverted!)
 *   0x30        : uint32_t    RGB LED colour (LE, 0x00RRGGBB)
 *   0x40        : uint16_t[8] calibration data (16 bytes)
 *   0x50        : int16_t[2]  offset-corrected 12-bit X, Y
 *   0x60        : int8_t[2]   offset-corrected 8-bit X, Y
 *   0xFE        : uint8_t     firmware version
 *   0xFF        : uint8_t     I2C address (R/W)
 *
 * Coordinate conventions after read:
 *   Raw 12-bit: 0-4095, centre ~2048
 *   Offset value: signed, centre = 0
 *   Button: isPressed() returns true when button is pressed (inverts the 0=pressed register)
 *
 * Usage:
 *   UnitJoystick2 joy;
 *   if (joy.begin(dev)) { ... }
 *   int16_t x, y; joy.readXY12(&x, &y);   // offset-corrected, centre=0
 *   bool btn = joy.isPressed();
 *   joy.setLed(0xFF0000);
 */
#pragma once

#include <UnitCommon.h>
#include <cstdint>

class UnitJoystick2 {
public:
    static constexpr uint8_t DEFAULT_ADDR = 0x63;

    // Pass a I2C controller device
    [[nodiscard]] bool begin(Device* dev, uint8_t addr = DEFAULT_ADDR);
    bool isPresent() const { return dev_ != nullptr; }

    // Read offset-corrected 12-bit XY. Centre = 0, range ~±2048.
    bool readXY12(int16_t* x, int16_t* y);

    // Read raw 12-bit XY (0-4095, unsigned). Centre ~2048.
    bool readXY12Raw(uint16_t* x, uint16_t* y);

    // Read offset-corrected 8-bit XY. Centre = 0, range ~±128.
    bool readXY8(int8_t* x, int8_t* y);

    // Button state. True = pressed (inverts hardware 0=pressed convention).
    bool isPressed() const;

    // Set the RGB LED colour (0x00RRGGBB). Returns false on I2C error.
    bool setLed(uint32_t rgb);

private:
    Device*  dev_  = nullptr;
    uint8_t  addr_ = DEFAULT_ADDR;

    static constexpr uint8_t REG_ADC_12BIT        = 0x00;
    static constexpr uint8_t REG_ADC_8BIT         = 0x10;
    static constexpr uint8_t REG_BUTTON           = 0x20;
    static constexpr uint8_t REG_RGB              = 0x30;
    static constexpr uint8_t REG_OFFSET_12BIT     = 0x50;
    static constexpr uint8_t REG_OFFSET_8BIT      = 0x60;
};
