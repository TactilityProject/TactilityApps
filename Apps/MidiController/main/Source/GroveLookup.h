#pragma once

#include <tactility/device.h>
#include <tactility/drivers/grove.h>

// Finds the I2C controller device exposed by the first grove port currently
// in GROVE_MODE_I2C, by checking each grove port's child device type
// (not by name - grove port names are not guaranteed, e.g. "port_a").
Device* findGroveI2cDevice();

// Same as findGroveI2cDevice() but for UART mode.
Device* findGroveUartDevice();
