/*
 * UnitCommon.h - shared I2C helpers for M5Stack STM32-based units
 *
 * All M5Stack Grove units with STM32 firmware require a STOP condition between
 * the register-pointer write and the data read (the STM32 HAL receive callback
 * populates tx_buffer after STOP, before the next START). Using a repeated-START
 * (the default i2c_controller_read_register) causes bus errors on every read.
 *
 * Pattern: write(reg) → STOP → vTaskDelay(2ms) → read(data)
 */
#pragma once

#include <tactility/device.h>
#include <tactility/drivers/i2c_controller.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>

static constexpr uint32_t UNIT_I2C_TIMEOUT_MS       = 20;
static constexpr uint32_t UNIT_I2C_PROBE_TIMEOUT_MS = 10;  // shorter timeout for presence checks
static constexpr uint32_t UNIT_STM32_READ_DELAY_MS  =  2;  // STM32 needs STOP + delay before read
static constexpr uint16_t UNIT_MAX_WRITE_PAYLOAD     = 32;  // max bytes in a single I2C write

// I2C addresses of the unit modules this library can drive (excludes PaHub itself
// and UART-only units like MIDI). Used by PaHub channel probing instead of a full
// 0x08-0x77 bus scan: the ESP-IDF i2c_master driver's probe op can wedge the bus
// FSM after heavy NACK traffic, so scanning only known addresses avoids that.
static constexpr uint8_t KNOWN_UNIT_ADDRS[] = {
    0x28, // UnitRfid2
    0x3E, // UnitLcd
    0x40, // UnitScroll
    0x41, // Unit8Encoder
    0x47, // UnitByteButton
    0x5F, // UnitCardKB2
    0x63, // UnitJoystick2
};

// Probe whether a device exists at the given address on the given I2C bus.
inline bool unitProbe(Device* dev, uint8_t addr) {
    return i2c_controller_has_device_at_address(
        dev, addr, pdMS_TO_TICKS(UNIT_I2C_PROBE_TIMEOUT_MS)) == ERROR_NONE;
}

// Write [reg, buf...] as a single transaction (STOP at end).
inline bool unitWriteReg(Device* dev, uint8_t addr, uint8_t reg,
                         const uint8_t* buf, uint16_t len) {
    if (len > UNIT_MAX_WRITE_PAYLOAD) return false;
    if (len > 0 && buf == nullptr) return false;
    uint8_t pkt[33];
    pkt[0] = reg;
    if (len) memcpy(pkt + 1, buf, len);
    return i2c_controller_write(dev, addr, pkt, (uint16_t)(len + 1),
                                pdMS_TO_TICKS(UNIT_I2C_TIMEOUT_MS)) == ERROR_NONE;
}

// Write register pointer only (no data payload) - convenience wrapper.
inline bool unitWriteRegPtr(Device* dev, uint8_t addr, uint8_t reg) {
    return i2c_controller_write(dev, addr, &reg, 1,
                                pdMS_TO_TICKS(UNIT_I2C_TIMEOUT_MS)) == ERROR_NONE;
}

// Read from a register: write pointer → STOP → 2ms delay → read.
inline bool unitReadReg(Device* dev, uint8_t addr, uint8_t reg,
                        uint8_t* buf, uint16_t len) {
    if (len > 0 && buf == nullptr) return false;
    if (!unitWriteRegPtr(dev, addr, reg)) return false;
    vTaskDelay(pdMS_TO_TICKS(UNIT_STM32_READ_DELAY_MS));
    return i2c_controller_read(dev, addr, buf, len,
                               pdMS_TO_TICKS(UNIT_I2C_TIMEOUT_MS)) == ERROR_NONE;
}
