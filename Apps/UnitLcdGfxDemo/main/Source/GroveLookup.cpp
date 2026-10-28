// SPDX-License-Identifier: Apache-2.0
#include "GroveLookup.h"

#include <tactility/drivers/i2c_controller.h>

namespace {

struct GroveChildSearch {
    Device* result;
};

bool onGroveDevice(Device* groveDevice, void* context) {
    auto* search = static_cast<GroveChildSearch*>(context);

    GroveMode mode;
    if (grove_get_mode(groveDevice, &mode) != ERROR_NONE) {
        return true;
    }
    if (mode != GROVE_MODE_I2C) {
        return true;
    }

    device_for_each_child(groveDevice, search, [](Device* child, void* ctx) -> bool {
        auto* state = static_cast<GroveChildSearch*>(ctx);
        if (device_get_type(child) == &I2C_CONTROLLER_TYPE && device_is_ready(child)) {
            state->result = child;
            return false;
        }
        return true;
    });

    return search->result == nullptr;
}

} // namespace

Device* findGroveI2cDevice() {
    GroveChildSearch search { nullptr };
    device_for_each_of_type(&GROVE_TYPE, &search, onGroveDevice);
    return search.result;
}
