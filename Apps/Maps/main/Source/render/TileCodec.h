// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace maps {

/**
 * Run-length coding of rendered RGB565 tiles for the SD card cache, where reading is the slow part.
 * The data is a sequence of 16-bit tokens: a token with the top bit set repeats the next pixel (token & 0x7FFF)
 * times, otherwise the token counts the literal pixels that follow.
 */
size_t encodeTile(const uint16_t* pixels, size_t count, uint16_t* out, size_t outCapacity);
bool decodeTile(const uint16_t* data, size_t dataCount, uint16_t* pixels, size_t count);

} // namespace maps
