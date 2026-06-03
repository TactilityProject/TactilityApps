#include <UnitByteButton.h>
#include <esp_log.h>

static constexpr auto* TAG = "UnitByteButton";

bool UnitByteButton::begin(Device* dev, uint8_t addr) {
    if (!dev || !device_is_ready(dev)) return false;
    if (!unitProbe(dev, addr)) {
        ESP_LOGW(TAG, "ByteButton not found at 0x%02X", addr);
        return false;
    }
    dev_  = dev;
    addr_ = addr;
    // Set LED show mode to user-defined (0x00) so our colour writes take effect
    uint8_t mode = 0x00;
    if (!unitWriteReg(dev_, addr_, REG_SHOW_MODE, &mode, 1)) {
        ESP_LOGW(TAG, "ByteButton show mode write failed at 0x%02X", addr_);
        dev_ = nullptr;
        return false;
    }
    // Turn all LEDs off and poison cache
    uint8_t off[32] = {};
    if (!unitWriteReg(dev_, addr_, REG_RGB888, off, 32)) {
        ESP_LOGW(TAG, "ByteButton LED init write failed at 0x%02X", addr_);
        dev_ = nullptr;
        return false;
    }
    memset(ledColor_, 0xFF, sizeof(ledColor_));
    ESP_LOGI(TAG, "ByteButton ready at 0x%02X", addr_);
    return true;
}

uint8_t UnitByteButton::readButtons(bool* ok) {
    if (!dev_) { if (ok) *ok = false; return 0; }
    uint8_t val = 0;
    bool success = unitReadReg(dev_, addr_, REG_STATUS, &val, 1);
    if (ok) *ok = success;
    return val;
}

bool UnitByteButton::readButton(uint8_t idx) {
    if (!dev_ || idx >= 8) return false;
    uint8_t val = 0;
    unitReadReg(dev_, addr_, (uint8_t)(REG_STATUS_8 + idx), &val, 1);
    return val != 0;
}

void UnitByteButton::setLed(uint8_t idx, uint32_t rgb) {
    if (!dev_ || idx >= 8) return;
    if (ledColor_[idx] == rgb) return;
    uint8_t buf[4] = {
        (uint8_t)( rgb        & 0xFF),
        (uint8_t)((rgb >>  8) & 0xFF),
        (uint8_t)((rgb >> 16) & 0xFF),
        0x00,
    };
    if (unitWriteReg(dev_, addr_, (uint8_t)(REG_RGB888 + idx * 4), buf, 4)) {
        ledColor_[idx] = rgb;
    }
}

void UnitByteButton::flushLeds(const uint32_t pending[8]) {
    if (!dev_) return;
    bool dirty = false;
    for (int i = 0; i < 8; i++)
        if (pending[i] != ledColor_[i]) { dirty = true; break; }
    if (!dirty) return;

    // Build 32-byte burst: 8 × 4-byte LE colour
    uint8_t buf[32];
    for (int i = 0; i < 8; i++) {
        buf[i*4+0] = (uint8_t)( pending[i]        & 0xFF);  // B
        buf[i*4+1] = (uint8_t)((pending[i] >>  8) & 0xFF);  // G
        buf[i*4+2] = (uint8_t)((pending[i] >> 16) & 0xFF);  // R
        buf[i*4+3] = 0x00;
    }
    if (unitWriteReg(dev_, addr_, REG_RGB888, buf, 32)) {
        for (int i = 0; i < 8; i++)
            ledColor_[i] = pending[i];
    } else {
        ESP_LOGW(TAG, "flushLeds write failed - cache not updated");
    }
}

void UnitByteButton::setAllLeds(uint32_t rgb) {
    if (!dev_) return;
    bool dirty = false;
    for (int i = 0; i < 8; i++)
        if (ledColor_[i] != rgb) { dirty = true; break; }
    if (!dirty) return;

    uint8_t buf[32];
    for (int i = 0; i < 8; i++) {
        buf[i*4+0] = (uint8_t)( rgb        & 0xFF);
        buf[i*4+1] = (uint8_t)((rgb >>  8) & 0xFF);
        buf[i*4+2] = (uint8_t)((rgb >> 16) & 0xFF);
        buf[i*4+3] = 0x00;
    }
    if (unitWriteReg(dev_, addr_, REG_RGB888, buf, 32)) {
        for (int i = 0; i < 8; i++) ledColor_[i] = rgb;
    } else {
        ESP_LOGW(TAG, "setAllLeds write failed - cache not updated");
    }
}
