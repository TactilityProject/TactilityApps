/*
 * UnitCardKB2.h - M5Stack CardKB V2 keyboard unit
 *
 * Supports two connection modes:
 *   I2C  (addr 0x5F, factory default) - via GROVE port
 *   UART (115200-8N-1)               - via GROVE port after Fn+Sym+2 mode switch
 *
 * I2C: plain 1-byte read returns ASCII of held key (0 = none).
 *
 * UART frame: AA [DATA_LEN=0x03] [KEY_ID] [KEY_STATE] [checksum]
 *   KEY_ID    = row*11 + col  (0-43, rows 0-3, cols 0-10)
 *   KEY_STATE = 0x01 pressed, 0x02 released
 *   checksum  = (DATA_LEN + KEY_ID + KEY_STATE) & 0xFF
 *
 * Special keys (UART translation by this driver; NOT available in I2C mode):
 *   Esc=0x1B  Del=0x08  Enter=0x0A  Space=0x20
 *   Fn+D=0x1E(up) Fn+X=0x1F(down) Fn+Z=0x1D(left) Fn+C=0x1C(right)
 *
 * I2C mode: Fn+D/X/Z/C and Fn+1 (Esc) produce no output - the firmware only
 * pushes regular ASCII into the I2C queue; Fn combos go to BLE HID directly.
 * UART mode: Fn combos work; firmware sends raw KEY_ID, driver translates.
 * BLE HID mode: USB HID keycodes (ESC=0x29, arrows=0x4F-0x52), handled by
 *               BluetoothHidHost → LV_KEY_*.
 *
 * Mode switch: Fn+Sym+1 = I2C, Fn+Sym+2 = UART (persists across reboot)
 */
#pragma once

#include <UnitCommon.h>
#include <tactility/drivers/uart_controller.h>
#include <cstdint>

class UnitCardKB2 {
public:
    enum class Mode { I2C, Uart };

    static constexpr uint8_t DEFAULT_ADDR = 0x5F;

    UnitCardKB2() = default;
    ~UnitCardKB2() { end(); }
    UnitCardKB2(const UnitCardKB2&) = delete;
    UnitCardKB2& operator=(const UnitCardKB2&) = delete;

    // Pass a I2C controller device
    // I2C mode - probe and initialise. Returns false if not present.
    [[nodiscard]] bool begin(Device* dev, uint8_t addr = DEFAULT_ADDR);

    // Pass a UART controller device
    // UART mode - set_config then open uart_controller.
    // Returns false if the device is null or open fails.
    [[nodiscard]] bool beginUart(Device* dev);

    void end();

    bool isPresent() const { return mode_ == Mode::I2C ? i2cDev_ != nullptr : uartDev_ != nullptr; }
    // Only meaningful when isPresent() returns true.
    // Call end() before switching modes (begin→beginUart or vice versa).
    Mode mode() const { return mode_; }

    // Returns the ASCII value of the currently/last pressed key, or 0 if none.
    // In UART mode: drains available bytes from the UART RX buffer, parses frames,
    // returns the ASCII of the most recently pressed key (releases are ignored).
    // In I2C mode: behaviour unchanged from before.
    char getKey();

    // Returns true if a key is currently held (I2C mode only; always false in UART mode).
    bool hasKey();

private:
    // I2C
    Device*  i2cDev_    = nullptr;
    uint8_t  addr_      = DEFAULT_ADDR;
    char     cachedKey_ = 0;

    // UART
    Device*  uartDev_   = nullptr;

    // Frame parser state
    enum class FrameState : uint8_t { WaitAA, WaitLen, WaitId, WaitState, WaitCsum };
    FrameState  frameState_ = FrameState::WaitAA;
    uint8_t     frameId_    = 0;
    uint8_t     frameKs_    = 0;

    // UART modifier state (tracked from key events)
    bool     capsLock_         = false;
    bool     symMode_          = false;
    bool     fnHeld_           = false;
    bool     oneShiftPending_  = false;  // Aa single-tap one-shot uppercase
    uint32_t lastAATimestamp_  = 0;      // ms timestamp of the Aa press that set oneShiftPending_

    Mode mode_ = Mode::I2C;

    char readFromI2C();
    char pollUart();
};
