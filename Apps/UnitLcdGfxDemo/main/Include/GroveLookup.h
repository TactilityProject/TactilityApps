// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <tactility/device.h>
#include <tactility/drivers/grove.h>

/** Finds the I2C controller device exposed by the first grove port currently in I2C mode. */
Device* findGroveI2cDevice();
