#pragma once

#include <tactility/device.h>
#include <tactility/drivers/grove.h>

// Finds the I2C controller device exposed by the first grove port currently
// in GROVE_MODE_I2C. Falls back to a direct "grove0_i2c" lookup so dedicated
// (non-grove) DTS setups using that fixed name keep working unchanged.
Device* findGroveI2cDevice();

// Same as findGroveI2cDevice() but for UART mode.
Device* findGroveUartDevice();
