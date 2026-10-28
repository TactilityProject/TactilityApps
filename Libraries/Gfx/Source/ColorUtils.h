// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <graphics/pixel_buffer.h>

#include <cstdint>

namespace gfx::detail {

inline uint16_t byteswap16(uint16_t value) {
    return static_cast<uint16_t>((value >> 8) | (value << 8));
}

// Swapping the 5-bit R and B fields is its own inverse, so this also converts BGR565 -> RGB565.
inline uint16_t swapRedBlue(uint16_t value) {
    const uint16_t r = (value >> 11) & 0x1F;
    const uint16_t g = (value >> 5) & 0x3F;
    const uint16_t b = value & 0x1F;
    return static_cast<uint16_t>((b << 11) | (g << 5) | r);
}

inline void toRgb888(uint16_t value, uint8_t out[3]) {
    const uint32_t r5 = (value >> 11) & 0x1F;
    const uint32_t g6 = (value >> 5) & 0x3F;
    const uint32_t b5 = value & 0x1F;
    out[0] = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
    out[1] = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
    out[2] = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
}

inline uint16_t rgb888To565(const uint8_t rgb[3]) {
    return static_cast<uint16_t>(((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) | (rgb[2] >> 3));
}

inline uint8_t toLuma(uint16_t value) {
    uint8_t rgb[3];
    toRgb888(value, rgb);
    const uint32_t luma = static_cast<uint32_t>(rgb[0]) * 299 + static_cast<uint32_t>(rgb[1]) * 587 + static_cast<uint32_t>(rgb[2]) * 114;
    return static_cast<uint8_t>(luma / 1000);
}

inline uint8_t toAverage(uint16_t value) {
    uint8_t rgb[3];
    toRgb888(value, rgb);
    return static_cast<uint8_t>((static_cast<uint16_t>(rgb[0]) + rgb[1] + rgb[2]) / 3);
}

inline uint8_t toGrayscale(uint16_t value, enum PixelBufferConversion conversion) {
    return conversion == PIXEL_BUFFER_CONVERSION_AVERAGE ? toAverage(value) : toLuma(value);
}

inline bool isInk(uint16_t value, enum PixelBufferConversion conversion) {
    if (conversion == PIXEL_BUFFER_CONVERSION_EXACT_BLACK) {
        return value != 0x0000;
    }
    return toGrayscale(value, conversion) >= 128;
}

/** Encodes an RGB565 colour into a fast RGB565-family format's native 16-bit value. */
inline uint16_t encode565(enum DisplayColorFormat format, uint16_t rgb565) {
    switch (format) {
        case DISPLAY_COLOR_FORMAT_RGB565_SWAPPED:
            return byteswap16(rgb565);
        case DISPLAY_COLOR_FORMAT_BGR565:
            return swapRedBlue(rgb565);
        case DISPLAY_COLOR_FORMAT_BGR565_SWAPPED:
            return byteswap16(swapRedBlue(rgb565));
        case DISPLAY_COLOR_FORMAT_RGB565:
        default:
            return rgb565;
    }
}

/** Decodes a fast RGB565-family native value back to RGB565. */
inline uint16_t decode565(enum DisplayColorFormat format, uint16_t native) {
    switch (format) {
        case DISPLAY_COLOR_FORMAT_RGB565_SWAPPED:
            return byteswap16(native);
        case DISPLAY_COLOR_FORMAT_BGR565:
            return swapRedBlue(native);
        case DISPLAY_COLOR_FORMAT_BGR565_SWAPPED:
            return swapRedBlue(byteswap16(native));
        case DISPLAY_COLOR_FORMAT_RGB565:
        default:
            return native;
    }
}

inline uint16_t blend565(uint16_t destination, uint16_t source, uint8_t alpha) {
    const int a = alpha;
    const int dr = (destination >> 11) & 0x1F;
    const int dg = (destination >> 5) & 0x3F;
    const int db = destination & 0x1F;
    const int sr = (source >> 11) & 0x1F;
    const int sg = (source >> 5) & 0x3F;
    const int sb = source & 0x1F;
    const int r = dr + (sr - dr) * a / 255;
    const int g = dg + (sg - dg) * a / 255;
    const int b = db + (sb - db) * a / 255;
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

} // namespace gfx::detail
