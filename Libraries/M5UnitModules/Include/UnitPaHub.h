/*
 * UnitPaHub.h - M5Stack PaHub v2 I2C multiplexer (TCA9548A, addr 0x70)
 *
 * 6-channel I2C mux. Selecting a channel routes the downstream bus to that
 * port only. Selecting channel 255 (or calling deselect()) disables all ports.
 *
 * Usage:
 *   UnitPaHub hub;
 *   if (hub.begin(dev)) {
 *       hub.select(0);           // enable channel 0
 *       enc.begin(dev);          // devices on channel 0 now reachable
 *       hub.deselect();          // disable all channels when done
 *   }
 *
 * Note: for devices that stay selected for extended periods (e.g. polling),
 * leave the channel selected - only call deselect() if you need to prevent
 * address conflicts between units on different channels.
 */
#pragma once

#include <UnitCommon.h>
#include <cstdint>

class UnitPaHub {
public:
    UnitPaHub() = default;
    UnitPaHub(const UnitPaHub&) = delete;
    UnitPaHub& operator=(const UnitPaHub&) = delete;

    static constexpr uint8_t DEFAULT_ADDR    = 0x70;
    static constexpr uint8_t NUM_CHANNELS    = 6;
    static constexpr uint8_t NO_CHANNEL      = 0xFF;

    // Pass a I2C controller device
    bool begin(Device* dev, uint8_t addr = DEFAULT_ADDR);
    bool isPresent() const { return dev_ != nullptr; }

    // Select a single channel (0-5). Returns false on error.
    bool select(uint8_t channel);

    // Disable all channels.
    bool deselect();

    uint8_t currentChannel() const { return channel_; }

private:
    Device*  dev_     = nullptr;
    uint8_t  addr_    = DEFAULT_ADDR;
    uint8_t  channel_ = NO_CHANNEL;
};
