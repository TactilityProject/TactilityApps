#include <Unit8Encoder.h>
#include <esp_log.h>

static constexpr auto* TAG = "Unit8Encoder";

static inline void packRgb(uint8_t* dst, uint32_t rgb) {
    dst[0] = (uint8_t)((rgb >> 16) & 0xFF);
    dst[1] = (uint8_t)((rgb >>  8) & 0xFF);
    dst[2] = (uint8_t)( rgb        & 0xFF);
}

bool Unit8Encoder::begin(Device* dev, uint8_t addr) {
    if (!dev || !device_is_ready(dev)) return false;
    if (!unitProbe(dev, addr)) {
        ESP_LOGW(TAG, "8Encoder not found at 0x%02X", addr);
        return false;
    }
    dev_  = dev;
    addr_ = addr;
    // Turn encoder LEDs off
    uint8_t off[ENCODER_LED_COUNT * 3] = {};
    if (!unitWriteReg(dev_, addr_, REG_LED, off, sizeof(off)))
        ESP_LOGW(TAG, "8Encoder LED init write failed at 0x%02X", addr_);
    // Turn switch LED off
    uint8_t offSw[3] = {};
    if (!unitWriteReg(dev_, addr_, REG_SWITCH_LED, offSw, 3))
        ESP_LOGW(TAG, "8Encoder switch LED init write failed at 0x%02X", addr_);
    // Poison cache so first flushLeds() always sends
    memset(ledColor_, 0xFF, sizeof(ledColor_));
    ESP_LOGI(TAG, "8Encoder ready at 0x%02X", addr_);
    return true;
}

bool Unit8Encoder::readAll(int32_t deltas[8], uint8_t buttons[8]) {
    if (!dev_) return false;
    for (int i = 0; i < 8; i++) {
        uint8_t buf[4] = {};
        if (!unitReadReg(dev_, addr_, (uint8_t)(REG_INCREMENT + i * 4), buf, 4)) {
            ESP_LOGW(TAG, "delta read failed ch%d", i);
            return false;
        }
        int32_t val;
        memcpy(&val, buf, 4);
        deltas[i] = val / 4;  // 4 pulses per detent
    }
    for (int i = 0; i < 8; i++) {
        uint8_t val = 0;
        if (!unitReadReg(dev_, addr_, (uint8_t)(REG_BUTTON + i), &val, 1)) {
            ESP_LOGW(TAG, "button read failed ch%d", i);
            return false;
        }
        buttons[i] = val;
    }
    return true;
}

bool Unit8Encoder::readSwitch(bool& state) {
    if (!dev_) return false;
    uint8_t val = 0;
    if (!unitReadReg(dev_, addr_, REG_SWITCH, &val, 1)) {
        ESP_LOGW(TAG, "switch read failed");
        return false;
    }
    state = (val != 0);
    return true;
}

void Unit8Encoder::setLed(uint8_t idx, uint32_t rgb) {
    if (!dev_ || idx >= LED_COUNT) return;
    if (ledColor_[idx] == rgb) return;
    uint8_t buf[3];
    packRgb(buf, rgb);
    uint8_t reg = (idx < ENCODER_LED_COUNT) ? (uint8_t)(REG_LED + idx * 3) : REG_SWITCH_LED;
    if (unitWriteReg(dev_, addr_, reg, buf, 3))
        ledColor_[idx] = rgb;
}

void Unit8Encoder::setSwitchLed(uint32_t rgb) {
    setLed(ENCODER_LED_COUNT, rgb);
}

void Unit8Encoder::flushLeds(const uint32_t pending[LED_COUNT]) {
    if (!dev_) return;

    // Encoder LEDs 0-7: batch write if any changed
    bool encDirty = false;
    for (int i = 0; i < ENCODER_LED_COUNT; i++)
        if (pending[i] != ledColor_[i]) { encDirty = true; break; }
    if (encDirty) {
        uint8_t buf[ENCODER_LED_COUNT * 3];
        for (int i = 0; i < ENCODER_LED_COUNT; i++) packRgb(buf + i * 3, pending[i]);
        if (unitWriteReg(dev_, addr_, REG_LED, buf, sizeof(buf))) {
            for (int i = 0; i < ENCODER_LED_COUNT; i++) ledColor_[i] = pending[i];
        } else {
            ESP_LOGW(TAG, "flushLeds encoder write failed");
        }
    }

    // Switch LED (index 8): write if changed
    if (pending[ENCODER_LED_COUNT] != ledColor_[ENCODER_LED_COUNT]) {
        uint8_t buf[3];
        packRgb(buf, pending[ENCODER_LED_COUNT]);
        if (unitWriteReg(dev_, addr_, REG_SWITCH_LED, buf, 3))
            ledColor_[ENCODER_LED_COUNT] = pending[ENCODER_LED_COUNT];
        else
            ESP_LOGW(TAG, "flushLeds switch LED write failed");
    }
}

void Unit8Encoder::setAllLeds(uint32_t rgb) {
    if (!dev_) return;

    // Encoder LEDs 0-7
    bool encDirty = false;
    for (int i = 0; i < ENCODER_LED_COUNT; i++)
        if (ledColor_[i] != rgb) { encDirty = true; break; }
    if (encDirty) {
        uint8_t buf[ENCODER_LED_COUNT * 3];
        for (int i = 0; i < ENCODER_LED_COUNT; i++) packRgb(buf + i * 3, rgb);
        if (unitWriteReg(dev_, addr_, REG_LED, buf, sizeof(buf))) {
            for (int i = 0; i < ENCODER_LED_COUNT; i++) ledColor_[i] = rgb;
        } else {
            ESP_LOGW(TAG, "setAllLeds encoder write failed");
        }
    }

    // Switch LED (index 8)
    if (ledColor_[ENCODER_LED_COUNT] != rgb) {
        uint8_t buf[3];
        packRgb(buf, rgb);
        if (unitWriteReg(dev_, addr_, REG_SWITCH_LED, buf, 3))
            ledColor_[ENCODER_LED_COUNT] = rgb;
        else
            ESP_LOGW(TAG, "setAllLeds switch LED write failed");
    }
}

