/*
 * UnitCommon.h - shared I2C helpers for M5Stack STM32-based units
 *
 * All M5Stack Grove units with STM32 firmware require a STOP condition between
 * the register-pointer write and the data read (the STM32 HAL receive callback
 * populates tx_buffer after STOP, before the next START). Using a repeated-START
 * (the default i2c_controller_read_register) causes bus errors on every read.
 *
 * Pattern: write(reg) → STOP → vTaskDelay(3ms) → read(data)
 */
#pragma once

#include <tactility/device.h>
#include <tactility/drivers/i2c_controller.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>

static constexpr uint32_t UNIT_I2C_TIMEOUT_MS       = 20;
static constexpr uint32_t UNIT_I2C_PROBE_TIMEOUT_MS = 10;  // shorter timeout for presence checks
static constexpr uint32_t UNIT_STM32_READ_DELAY_MS  =  3;  // STM32 needs STOP + delay before read
static constexpr uint16_t UNIT_MAX_WRITE_PAYLOAD     = 32;  // max bytes in a single I2C write

// A single probe can fail while a slave is still finishing its previous
// transaction (the 8Encoder's STM32 re-arms its I2C listener in the STOP
// callback). Retry a few times, but accept the first ACK: requiring every
// attempt to ACK rejects healthy STM32 units. Callers still validate a hit
// with a real register access in begin().
//
// Five attempts matter for the Scroll/ByteButton/Joystick2 units: their first
// version read after a PaHub channel change is often lost while the downstream
// STM32 re-arms. Retrying with a 2 ms gap avoids a false "not found" on an
// otherwise healthy channel.
static constexpr int UNIT_I2C_PROBE_ATTEMPTS = 5;
static constexpr uint32_t UNIT_I2C_PROBE_RETRY_DELAY_MS = 2;

// Identity registers used by unitProbeOnce().
static constexpr uint8_t UNIT_CARDKB2_VERSION_REG = 0xF1;  // returns 0x02 on current firmware
static constexpr uint8_t UNIT_RFID2_VERSION_REG   = 0x37;  // MFRC522/WS1850S VersionReg

// I2C addresses of the unit modules this library can drive (excludes PaHub itself
// and UART-only units like MIDI). Used by the PaHub channel scanner instead of a
// full 0x08-0x77 bus scan: the ESP-IDF i2c_master probe op is unreliable behind a
// mux, so only known addresses are checked. Common display/input units are listed
// first so a scan that finds one can stop early. Every entry has an identity read
// (no write-only probe), which is what stops a floating mux channel from
// producing false ACKs.
static constexpr uint8_t KNOWN_UNIT_ADDRS[] = {
    0x41, // Unit8Encoder (the most common PaHub unit, so find it quickly)
    0x3E, // UnitLcd
    0x40, // UnitScroll
    0x47, // UnitByteButton
    0x63, // UnitJoystick2
    0x28, // UnitRfid2
    0x5F, // UnitCardKB2
};

// Probe whether a device exists at the given address on the given I2C bus.
//
// Do NOT use i2c_controller_has_device_at_address() here. On ESP-IDF 6.1 its
// i2c_master_probe() path can report ESP_OK for an absent address when the bus
// FSM is left in a non-error state; behind a PaHub that turned every address on
// every channel into a false ACK. A real transaction gives a reliable NACK.
//
// The transaction is chosen so it is harmless and identifies the device:
//   - PaHub: read the control register.
//   - 8Encoder/Scroll/ByteButton/Joystick2: read firmware version register
//     0xFE and require 1-15.
//   - LCD: read the four-byte ID (command 0x04); require 0x77 0x89.
//   - CardKB2: read firmware version register 0xF1 and require 1-15. This also
//     avoids consuming a queued key byte, which a plain read would do.
//   - RFID2: read the MFRC522/WS1850S VersionReg (0x37) and require a known
//     WS1850S or MFRC522-family value.
inline bool unitProbeOnce(Device* dev, uint8_t addr) {
    uint8_t value = 0;
    switch (addr) {
        case 0x70: // UnitPaHub - read the control register
            return i2c_controller_read(dev, addr, &value, 1,
                                       pdMS_TO_TICKS(UNIT_I2C_PROBE_TIMEOUT_MS)) == ERROR_NONE;
        case 0x41: // Unit8Encoder
        case 0x40: // UnitScroll
        case 0x47: // UnitByteButton
        case 0x63: { // UnitJoystick2
            // These STM32 units expose a firmware version at 0xFE. A
            // write-only probe is not enough behind a floating PaHub channel:
            // an absent device can look ACKed on every address, and a real
            // version value is a much stronger identity check.
            uint8_t reg = 0xFE;
            if (i2c_controller_write(dev, addr, &reg, 1,
                                     pdMS_TO_TICKS(UNIT_I2C_PROBE_TIMEOUT_MS)) != ERROR_NONE) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(UNIT_STM32_READ_DELAY_MS));
            if (i2c_controller_read(dev, addr, &value, 1,
                                    pdMS_TO_TICKS(UNIT_I2C_PROBE_TIMEOUT_MS)) != ERROR_NONE) {
                return false;
            }
            return value >= 1 && value <= 15;
        }
        case 0x3E: { // UnitLcd - read the 4-byte ID (0x77 0x89 ...)
            uint8_t cmd = 0x04;
            if (i2c_controller_write(dev, addr, &cmd, 1,
                                     pdMS_TO_TICKS(UNIT_I2C_PROBE_TIMEOUT_MS)) != ERROR_NONE) {
                return false;
            }
            uint8_t id[4] = {};
            if (i2c_controller_read(dev, addr, id, sizeof(id),
                                    pdMS_TO_TICKS(UNIT_I2C_PROBE_TIMEOUT_MS)) != ERROR_NONE) {
                return false;
            }
            return id[0] == 0x77 && id[1] == 0x89;
        }
        case 0x5F: { // UnitCardKB2 - read the firmware version register
            uint8_t reg = UNIT_CARDKB2_VERSION_REG;
            if (i2c_controller_write(dev, addr, &reg, 1,
                                     pdMS_TO_TICKS(UNIT_I2C_PROBE_TIMEOUT_MS)) != ERROR_NONE) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(UNIT_STM32_READ_DELAY_MS));
            if (i2c_controller_read(dev, addr, &value, 1,
                                    pdMS_TO_TICKS(UNIT_I2C_PROBE_TIMEOUT_MS)) != ERROR_NONE) {
                return false;
            }
            return value >= 1 && value <= 15;
        }
        case 0x28: { // UnitRfid2 - read the MFRC522/WS1850S VersionReg
            uint8_t reg = UNIT_RFID2_VERSION_REG;
            if (i2c_controller_write(dev, addr, &reg, 1,
                                     pdMS_TO_TICKS(UNIT_I2C_PROBE_TIMEOUT_MS)) != ERROR_NONE) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(UNIT_STM32_READ_DELAY_MS));
            if (i2c_controller_read(dev, addr, &value, 1,
                                    pdMS_TO_TICKS(UNIT_I2C_PROBE_TIMEOUT_MS)) != ERROR_NONE) {
                return false;
            }
            // 0x15 is the version reported by the WS1850S in the RFID2 Unit.
            // 0x88/0x90/0x91/0x92/0x12 are MFRC522/PN512 family values.
            return value == 0x15 || value == 0x88 || value == 0x90 ||
                   value == 0x91 || value == 0x92 || value == 0x12;
        }
        default: { // STM32 register-based units: set the register pointer only
            uint8_t reg = 0x00;
            return i2c_controller_write(dev, addr, &reg, 1,
                                        pdMS_TO_TICKS(UNIT_I2C_PROBE_TIMEOUT_MS)) == ERROR_NONE;
        }
    }
}

inline bool unitProbe(Device* dev, uint8_t addr) {
    for (int i = 0; i < UNIT_I2C_PROBE_ATTEMPTS; i++) {
        if (unitProbeOnce(dev, addr)) {
            return true;
        }
        if (i + 1 < UNIT_I2C_PROBE_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(UNIT_I2C_PROBE_RETRY_DELAY_MS));
        }
    }
    return false;
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

// Read from a register: write pointer → STOP → 3ms delay → read.
inline bool unitReadReg(Device* dev, uint8_t addr, uint8_t reg,
                        uint8_t* buf, uint16_t len) {
    if (len > 0 && buf == nullptr) return false;
    if (!unitWriteRegPtr(dev, addr, reg)) return false;
    vTaskDelay(pdMS_TO_TICKS(UNIT_STM32_READ_DELAY_MS));
    return i2c_controller_read(dev, addr, buf, len,
                               pdMS_TO_TICKS(UNIT_I2C_TIMEOUT_MS)) == ERROR_NONE;
}
