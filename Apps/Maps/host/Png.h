// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

/** Writes RGB565 pixels as a 24-bit PNG file. */
bool writePngRgb565(const char* path, const uint16_t* pixels, int width, int height);
