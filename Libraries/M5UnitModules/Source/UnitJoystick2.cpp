#include <UnitJoystick2.h>
#include <esp_log.h>
#include <cstring>

static constexpr auto* TAG = "UnitJoystick2";

bool UnitJoystick2::begin(Device* dev, uint8_t addr) {
    if (!dev || !device_is_ready(dev)) return false;
    if (!unitProbe(dev, addr)) {
        ESP_LOGW(TAG, "Joystick2 not found at 0x%02X", addr);
        return false;
    }
    dev_  = dev;
    addr_ = addr;
    ESP_LOGI(TAG, "Joystick2 ready at 0x%02X", addr_);
    return true;
}

bool UnitJoystick2::readXY12(int16_t* x, int16_t* y) {
    if (!dev_ || !x || !y) return false;
    uint8_t buf[4] = {};
    if (!unitReadReg(dev_, addr_, REG_OFFSET_12BIT, buf, 4)) return false;
    memcpy(x, &buf[0], 2);
    memcpy(y, &buf[2], 2);
    return true;
}

bool UnitJoystick2::readXY12Raw(uint16_t* x, uint16_t* y) {
    if (!dev_ || !x || !y) return false;
    uint8_t buf[4] = {};
    if (!unitReadReg(dev_, addr_, REG_ADC_12BIT, buf, 4)) return false;
    memcpy(x, &buf[0], 2);
    memcpy(y, &buf[2], 2);
    return true;
}

bool UnitJoystick2::readXY8(int8_t* x, int8_t* y) {
    if (!dev_ || !x || !y) return false;
    uint8_t buf[2] = {};
    if (!unitReadReg(dev_, addr_, REG_OFFSET_8BIT, buf, 2)) return false;
    *x = (int8_t)buf[0];
    *y = (int8_t)buf[1];
    return true;
}

bool UnitJoystick2::isPressed() const {
    if (!dev_) return false;
    uint8_t val = 1;
    if (!unitReadReg(dev_, addr_, REG_BUTTON, &val, 1)) {
        ESP_LOGW(TAG, "button read failed at 0x%02X", addr_);
    }
    return val == 0;  // hardware: 0=pressed, 1=released
}

bool UnitJoystick2::setLed(uint32_t rgb) {
    if (!dev_) return false;
    // LE uint32_t: 0x00RRGGBB → wire bytes [BB, GG, RR, 00]
    uint8_t buf[4] = {
        (uint8_t)( rgb        & 0xFF),
        (uint8_t)((rgb >>  8) & 0xFF),
        (uint8_t)((rgb >> 16) & 0xFF),
        0x00
    };
    return unitWriteReg(dev_, addr_, REG_RGB, buf, 4);
}
