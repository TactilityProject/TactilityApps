#include "GroveLookup.h"
#include <tactility/drivers/i2c_controller.h>
#include <tactility/drivers/uart_controller.h>

namespace {

struct GroveChildSearch {
    const DeviceType* childType;
    Device* result;
};

bool onGroveDevice(Device* groveDevice, void* context) {
    auto* search = static_cast<GroveChildSearch*>(context);

    GroveMode mode;
    if (grove_get_mode(groveDevice, &mode) != ERROR_NONE) return true;
    if (mode != (search->childType == &I2C_CONTROLLER_TYPE ? GROVE_MODE_I2C : GROVE_MODE_UART)) return true;

    device_for_each_child(groveDevice, search, [](Device* child, void* ctx) -> bool {
        auto* s = static_cast<GroveChildSearch*>(ctx);
        if (device_get_type(child) == s->childType && device_is_ready(child)) {
            s->result = child;
            return false; // stop - found it
        }
        return true;
    });

    return search->result == nullptr; // stop outer iteration once found
}

Device* findGroveChild(const DeviceType* childType) {
    GroveChildSearch search{childType, nullptr};
    device_for_each_of_type(&GROVE_TYPE, &search, onGroveDevice);
    return search.result;
}

} // namespace

Device* findGroveI2cDevice() {
    return findGroveChild(&I2C_CONTROLLER_TYPE);
}

Device* findGroveUartDevice() {
    return findGroveChild(&UART_CONTROLLER_TYPE);
}
