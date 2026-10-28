#include <UnitPaHub.h>
#include <esp_log.h>

static constexpr auto* TAG = "UnitPaHub";

bool UnitPaHub::begin(Device* dev, uint8_t addr) {
    dev_     = nullptr;
    addr_    = 0;
    channel_ = NO_CHANNEL;
    if (!dev || !device_is_ready(dev)) return false;
    if (!unitProbe(dev, addr)) {
        ESP_LOGD(TAG, "PaHub not found at 0x%02X", addr);
        return false;
    }
    dev_  = dev;
    addr_ = addr;
    // Disable all channels on init
    if (!deselect()) {
        ESP_LOGE(TAG, "PaHub deselect failed at 0x%02X", addr_);
        dev_  = nullptr;
        addr_ = 0;
        return false;
    }
    ESP_LOGI(TAG, "PaHub ready at 0x%02X", addr_);
    return true;
}

bool UnitPaHub::readChannel(uint8_t& mask) {
    mask = 0;
    if (!dev_) return false;
    return i2c_controller_read(dev_, addr_, &mask, 1,
                               pdMS_TO_TICKS(UNIT_I2C_TIMEOUT_MS)) == ERROR_NONE;
}

bool UnitPaHub::select(uint8_t channel) {
    if (!dev_ || channel >= NUM_CHANNELS) return false;
    // Writing the same mask again briefly disconnects and reconnects the
    // downstream bus, which can make the next transaction fail. Skip it.
    if (channel_ == channel) return true;
    // PCA9548AP: write one byte - bit i set = channel i enabled.
    const uint8_t mask = (uint8_t)(1u << channel);
    bool wrote = false;
    for (int attempt = 0; attempt < 3; attempt++) {
        wrote = i2c_controller_write(dev_, addr_, &mask, 1,
                                     pdMS_TO_TICKS(UNIT_I2C_TIMEOUT_MS)) == ERROR_NONE;
        if (wrote) break;
        if (attempt + 1 < 3) vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!wrote) {
        // The hardware state is unknown, so force the next select() to write
        // the mask again instead of trusting the cache.
        channel_ = NO_CHANNEL;
        ESP_LOGW(TAG, "PaHub select CH%u write failed at 0x%02X",
                 (unsigned)channel, addr_);
        return false;
    }
    channel_ = channel;
    // Give the newly connected downstream bus a moment to settle. One
    // millisecond was not enough for some STM32 units: their first version
    // read after a channel change could NACK while the listener re-armed.
    vTaskDelay(pdMS_TO_TICKS(3));

    // Read the control register back for diagnostics only. It cannot be a hard
    // gate: once a channel with an STM32 unit is selected, that unit can hold
    // SCL low (clock stretching) and block the upstream readback even though
    // the mux latched correctly. The PaHub test logs this value; identity
    // checks in Unit8Encoder/UnitLcd prevent a wrong channel from being used.
    uint8_t readback = 0;
    if (readChannel(readback) && readback != mask) {
        ESP_LOGD(TAG, "PaHub select CH%u readback 0x%02X (expected 0x%02X)",
                 (unsigned)channel, readback, mask);
    }
    return true;
}

bool UnitPaHub::deselect() {
    if (!dev_) return false;
    uint8_t mask = 0x00;
    uint8_t readback = 0;
    bool readback_ok = false;
    for (int attempt = 0; attempt < 3; attempt++) {
        bool wrote = i2c_controller_write(dev_, addr_, &mask, 1,
                                          pdMS_TO_TICKS(UNIT_I2C_TIMEOUT_MS)) == ERROR_NONE;
        if (wrote) {
            vTaskDelay(pdMS_TO_TICKS(1));
            readback_ok = readChannel(readback);
            if (readback_ok && readback == 0) {
                channel_ = NO_CHANNEL;
                return true;
            }
        }
        if (attempt + 1 < 3) vTaskDelay(pdMS_TO_TICKS(1));
    }
    // Invalidate even on failure: a partial write may have changed the
    // hardware, and select() must not skip the next write because of a stale
    // cached channel.
    channel_ = NO_CHANNEL;
    if (readback_ok) {
        ESP_LOGW(TAG, "PaHub deselect failed at 0x%02X (readback 0x%02X)", addr_, readback);
    } else {
        ESP_LOGW(TAG, "PaHub deselect failed at 0x%02X (readback failed)", addr_);
    }
    return false;
}
