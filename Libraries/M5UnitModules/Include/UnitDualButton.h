/*
 * UnitDualButton.h - M5Stack Dual Button Unit (GPIO, Port B)
 *
 * Two independent momentary push buttons connected to GPIO pins.
 * Buttons are active-low - pressing pulls the pin to GND.
 * Internal pull-up resistors are enabled by the driver.
 *
 * Typical Port B pin assignment (M5Stack Core2):
 *   Button A (red)  - GPIO 36
 *   Button B (blue) - GPIO 26
 *
 * Usage:
 *   UnitDualButton btn;
 *   Device* gpioCtrl = device_find_by_name("gpio0");
 *   if (btn.begin(gpioCtrl, 36, 26)) {
 *       bool a = btn.isButtonAPressed();
 *       bool b = btn.isButtonBPressed();
 *   }
 *   // When done: btn.end() or let destructor release pins
 */
#pragma once

#include <tactility/device.h>
#include <tactility/drivers/gpio_controller.h>
#include <cstdint>

class UnitDualButton {
public:
    ~UnitDualButton();

    UnitDualButton() = default;
    UnitDualButton(const UnitDualButton&) = delete;
    UnitDualButton& operator=(const UnitDualButton&) = delete;
    UnitDualButton(UnitDualButton&&) = delete;
    UnitDualButton& operator=(UnitDualButton&&) = delete;

    // Pass a GPIO controller device
    // Acquire and configure both GPIO pins as inputs with pull-up.
    // controller: GPIO controller device (e.g. device_find_by_name("gpio0"))
    // pinA / pinB: physical GPIO pin numbers
    bool begin(Device* controller, gpio_pin_t pinA, gpio_pin_t pinB);

    // Release GPIO descriptors. Safe to call multiple times.
    void end();

    bool isPresent() const { return ready_; }

    // True when button A is pressed (active-low: pin reads low).
    bool isButtonAPressed() const;

    // True when button B is pressed (active-low: pin reads low).
    bool isButtonBPressed() const;

private:
    GpioDescriptor* descA_ = nullptr;
    GpioDescriptor* descB_ = nullptr;
    bool ready_ = false;

    static bool readPin(GpioDescriptor* desc);
};
