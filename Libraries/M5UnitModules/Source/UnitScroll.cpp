#include <UnitScroll.h>
#include <esp_log.h>

static constexpr auto* TAG = "UnitScroll";

bool UnitScroll::begin(Device* dev, uint8_t addr) {
    if (!dev || !device_is_ready(dev)) return false;
    if (!unitProbe(dev, addr)) {
        ESP_LOGW(TAG, "Scroll not found at 0x%02X", addr);
        return false;
    }
    dev_  = dev;
    addr_ = addr;
    ESP_LOGI(TAG, "Scroll ready at 0x%02X", addr_);
    return true;
}

int16_t UnitScroll::readDelta() {
    if (!dev_) return 0;
    int16_t val = 0;
    if (!unitReadReg(dev_, addr_, REG_INC_ENCODER, (uint8_t*)&val, 2))
        ESP_LOGW(TAG, "readDelta failed at 0x%02X", addr_);
    return val;
}

int16_t UnitScroll::readAbsolute() const {
    if (!dev_) return 0;
    int16_t val = 0;
    if (!unitReadReg(dev_, addr_, REG_ENCODER, (uint8_t*)&val, 2))
        ESP_LOGW(TAG, "readAbsolute failed at 0x%02X", addr_);
    return val;
}

bool UnitScroll::isPressed() const {
    if (!dev_) return false;
    uint8_t val = 1;
    if (!unitReadReg(dev_, addr_, REG_BUTTON, &val, 1))
        ESP_LOGW(TAG, "isPressed read failed at 0x%02X", addr_);
    return val == 0;  // hardware: 0=pressed, 1=released
}

void UnitScroll::setLed(uint32_t rgb) {
    if (!dev_) return;
    // Wire format: [index=0, R, G, B]
    uint8_t buf[4] = {
        0x00,
        (uint8_t)((rgb >> 16) & 0xFF),  // R
        (uint8_t)((rgb >>  8) & 0xFF),  // G
        (uint8_t)( rgb        & 0xFF),  // B
    };
    unitWriteReg(dev_, addr_, REG_LED, buf, 4);
}

void UnitScroll::resetEncoder() {
    if (!dev_) return;
    uint8_t one = 1;
    unitWriteReg(dev_, addr_, REG_RESET, &one, 1);
}
