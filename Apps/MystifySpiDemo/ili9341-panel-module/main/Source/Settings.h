// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <ili9341panel.h>

namespace ili9341panel {

/** Loads the saved settings, falling back to the defaults per field. */
error_t loadSettings(struct Ili9341PanelConfig& config);

/** Writes every field to the properties file. */
error_t saveSettings(const struct Ili9341PanelConfig& config);

} // namespace ili9341panel
