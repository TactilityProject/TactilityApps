#include <UnitPaHub.h>
#include <esp_log.h>

static constexpr auto* TAG = "UnitPaHub";

bool UnitPaHub::begin(Device* dev, uint8_t addr) {
    if (!dev || !device_is_ready(dev)) return false;
    if (!unitProbe(dev, addr)) {
        ESP_LOGW(TAG, "PaHub not found at 0x%02X", addr);
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

bool UnitPaHub::select(uint8_t channel) {
    if (!dev_ || channel >= NUM_CHANNELS) return false;
    // TCA9548A: write one byte - bit i set = channel i enabled
    uint8_t mask = (uint8_t)(1u << channel);
    bool ok = i2c_controller_write(dev_, addr_, &mask, 1,
                                   pdMS_TO_TICKS(UNIT_I2C_TIMEOUT_MS)) == ERROR_NONE;
    if (ok) channel_ = channel;
    return ok;
}

bool UnitPaHub::deselect() {
    if (!dev_) return false;
    uint8_t mask = 0x00;
    bool ok = i2c_controller_write(dev_, addr_, &mask, 1,
                                   pdMS_TO_TICKS(UNIT_I2C_TIMEOUT_MS)) == ERROR_NONE;
    if (ok) channel_ = NO_CHANNEL;
    return ok;
}
