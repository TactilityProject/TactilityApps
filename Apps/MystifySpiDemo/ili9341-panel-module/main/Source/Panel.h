// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <ili9341panel.h>

namespace ili9341panel {

/** Creates the SPI bus and the display device from @a config. Does nothing when disabled. */
error_t startPanel(const struct Ili9341PanelConfig& config);

/** Stops and tears down the device and the bus. */
error_t stopPanel();

bool isPanelActive();

/** @return a new device reference, or nullptr when the panel is not active. */
struct Device* getPanelDevice();

} // namespace ili9341panel
