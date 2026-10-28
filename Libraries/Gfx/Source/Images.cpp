// SPDX-License-Identifier: Apache-2.0
#include <gfx/Surface.h>

#include "ColorUtils.h"

#include <cstring>

namespace gfx {

namespace {

bool isFastRgb565Format(enum DisplayColorFormat format) {
    switch (format) {
        case DISPLAY_COLOR_FORMAT_RGB565:
        case DISPLAY_COLOR_FORMAT_RGB565_SWAPPED:
        case DISPLAY_COLOR_FORMAT_BGR565:
        case DISPLAY_COLOR_FORMAT_BGR565_SWAPPED:
            return true;
        default:
            return false;
    }
}

int bytesPerPixel(enum DisplayColorFormat format) {
    switch (format) {
        case DISPLAY_COLOR_FORMAT_RGB888:
            return 3;
        case DISPLAY_COLOR_FORMAT_GRAYSCALE8:
            return 1;
        case DISPLAY_COLOR_FORMAT_MONOCHROME:
            return 0;
        default:
            return 2;
    }
}

} // namespace

void Surface::blit(const struct PixelBuffer& source, int sourceX, int sourceY, int width, int height, int destX, int destY, enum PixelBufferConversion conversion) {
    const int sourceWidth = pixel_buffer_get_width(&source);
    const int sourceHeight = pixel_buffer_get_height(&source);

    int sx = sourceX;
    int sy = sourceY;
    int dx = destX;
    int dy = destY;
    int w = width;
    int h = height;

    if (sx < 0) {
        dx -= sx;
        w += sx;
        sx = 0;
    }
    if (sy < 0) {
        dy -= sy;
        h += sy;
        sy = 0;
    }
    if (sx + w > sourceWidth) {
        w = sourceWidth - sx;
    }
    if (sy + h > sourceHeight) {
        h = sourceHeight - sy;
    }

    if (dx < clip_.x0) {
        const int delta = clip_.x0 - dx;
        sx += delta;
        w -= delta;
        dx = clip_.x0;
    }
    if (dy < clip_.y0) {
        const int delta = clip_.y0 - dy;
        sy += delta;
        h -= delta;
        dy = clip_.y0;
    }
    if (dx + w > clip_.x1) {
        w = clip_.x1 - dx;
    }
    if (dy + h > clip_.y1) {
        h = clip_.y1 - dy;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    markDirtyRect(dx, dy, dx + w, dy + h);

    const auto sourceFormat = pixel_buffer_get_format(&source);
    if (sourceFormat == format_ && sourceFormat != DISPLAY_COLOR_FORMAT_MONOCHROME) {
        const int bpp = bytesPerPixel(sourceFormat);
        const auto* sourceData = static_cast<const uint8_t*>(pixel_buffer_get_data(const_cast<struct PixelBuffer*>(&source)));
        const std::size_t sourceStride = pixel_buffer_get_stride_bytes(&source);
        const int localX = dx - bounds_.x0;
        const int localY = dy - bounds_.y0;

        for (int row = 0; row < h; row++) {
            std::memcpy(
                data_ + static_cast<std::size_t>(localY + row) * stride_ + static_cast<std::size_t>(localX) * bpp,
                sourceData + static_cast<std::size_t>(sy + row) * sourceStride + static_cast<std::size_t>(sx) * bpp,
                static_cast<std::size_t>(w) * bpp
            );
        }
        return;
    }

    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            const uint16_t rgb565 = pixel_buffer_get_pixel_rgb565(&source, sx + col, sy + row);
            writePixel(dx + col - bounds_.x0, dy + row - bounds_.y0, rgb565, conversion);
        }
    }
}

void Surface::blitTransparent(const struct PixelBuffer& source, int sourceX, int sourceY, int width, int height, int destX, int destY, uint16_t transparentRgb565, enum PixelBufferConversion conversion) {
    const int sourceWidth = pixel_buffer_get_width(&source);
    const int sourceHeight = pixel_buffer_get_height(&source);

    int sx = sourceX;
    int sy = sourceY;
    int dx = destX;
    int dy = destY;
    int w = width;
    int h = height;

    if (sx < 0) {
        dx -= sx;
        w += sx;
        sx = 0;
    }
    if (sy < 0) {
        dy -= sy;
        h += sy;
        sy = 0;
    }
    if (sx + w > sourceWidth) {
        w = sourceWidth - sx;
    }
    if (sy + h > sourceHeight) {
        h = sourceHeight - sy;
    }

    if (dx < clip_.x0) {
        const int delta = clip_.x0 - dx;
        sx += delta;
        w -= delta;
        dx = clip_.x0;
    }
    if (dy < clip_.y0) {
        const int delta = clip_.y0 - dy;
        sy += delta;
        h -= delta;
        dy = clip_.y0;
    }
    if (dx + w > clip_.x1) {
        w = clip_.x1 - dx;
    }
    if (dy + h > clip_.y1) {
        h = clip_.y1 - dy;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    markDirtyRect(dx, dy, dx + w, dy + h);

    const auto sourceFormat = pixel_buffer_get_format(&source);
    if (sourceFormat == format_ && isFastRgb565Format(sourceFormat)) {
        const uint16_t transparentNative = detail::encode565(sourceFormat, transparentRgb565);
        const auto* sourceData = static_cast<const uint8_t*>(pixel_buffer_get_data(const_cast<struct PixelBuffer*>(&source)));
        const std::size_t sourceStride = pixel_buffer_get_stride_bytes(&source);
        const int localX = dx - bounds_.x0;
        const int localY = dy - bounds_.y0;

        for (int row = 0; row < h; row++) {
            const auto* sourceRow = reinterpret_cast<const uint16_t*>(
                sourceData + static_cast<std::size_t>(sy + row) * sourceStride
            );
            auto* destinationRow = reinterpret_cast<uint16_t*>(
                data_ + static_cast<std::size_t>(localY + row) * stride_
            );
            for (int col = 0; col < w; col++) {
                const uint16_t value = sourceRow[sx + col];
                if (value != transparentNative) {
                    destinationRow[localX + col] = value;
                }
            }
        }
        return;
    }

    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            const uint16_t rgb565 = pixel_buffer_get_pixel_rgb565(&source, sx + col, sy + row);
            if (rgb565 != transparentRgb565) {
                writePixel(dx + col - bounds_.x0, dy + row - bounds_.y0, rgb565, conversion);
            }
        }
    }
}

void Surface::drawMask(const uint8_t* mask, int maskStride, int width, int height, int x, int y, uint16_t rgb565, bool msbFirst) {
    if (mask == nullptr || width <= 0 || height <= 0) {
        return;
    }
    if (maskStride <= 0) {
        maskStride = (width + 7) / 8;
    }

    int maskX = 0;
    int maskY = 0;
    int dx = x;
    int dy = y;
    int w = width;
    int h = height;

    if (dx < clip_.x0) {
        const int delta = clip_.x0 - dx;
        maskX += delta;
        w -= delta;
        dx = clip_.x0;
    }
    if (dy < clip_.y0) {
        const int delta = clip_.y0 - dy;
        maskY += delta;
        h -= delta;
        dy = clip_.y0;
    }
    if (dx + w > clip_.x1) {
        w = clip_.x1 - dx;
    }
    if (dy + h > clip_.y1) {
        h = clip_.y1 - dy;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    markDirtyRect(dx, dy, dx + w, dy + h);

    for (int row = 0; row < h; row++) {
        const uint8_t* maskRow = mask + static_cast<std::size_t>(maskY + row) * maskStride;
        for (int col = 0; col < w; col++) {
            const int sourceCol = maskX + col;
            const uint8_t value = maskRow[sourceCol >> 3];
            const uint8_t bit = msbFirst
                ? static_cast<uint8_t>(0x80U >> (sourceCol & 7))
                : static_cast<uint8_t>(1U << (sourceCol & 7));
            if (value & bit) {
                plotRaw(dx + col, dy + row, rgb565);
            }
        }
    }
}

void Surface::drawAlphaMask(const uint8_t* alpha, int alphaStride, int width, int height, int x, int y, uint16_t rgb565) {
    if (alpha == nullptr || width <= 0 || height <= 0) {
        return;
    }
    if (alphaStride <= 0) {
        alphaStride = width;
    }

    int maskX = 0;
    int maskY = 0;
    int dx = x;
    int dy = y;
    int w = width;
    int h = height;

    if (dx < clip_.x0) {
        const int delta = clip_.x0 - dx;
        maskX += delta;
        w -= delta;
        dx = clip_.x0;
    }
    if (dy < clip_.y0) {
        const int delta = clip_.y0 - dy;
        maskY += delta;
        h -= delta;
        dy = clip_.y0;
    }
    if (dx + w > clip_.x1) {
        w = clip_.x1 - dx;
    }
    if (dy + h > clip_.y1) {
        h = clip_.y1 - dy;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    markDirtyRect(dx, dy, dx + w, dy + h);

    for (int row = 0; row < h; row++) {
        const uint8_t* alphaRow = alpha + static_cast<std::size_t>(maskY + row) * alphaStride;
        for (int col = 0; col < w; col++) {
            const uint8_t value = alphaRow[maskX + col];
            if (value != 0) {
                blendPixelRaw(dx + col, dy + row, rgb565, value);
            }
        }
    }
}

void Surface::blendPixelRaw(int x, int y, uint16_t rgb565, uint8_t alpha) {
    if (alpha == 0 || !clip_.contains(x, y)) {
        return;
    }

    const int localX = x - bounds_.x0;
    const int localY = y - bounds_.y0;
    if (alpha == 255) {
        writePixel(localX, localY, rgb565);
        return;
    }

    const uint16_t destination = readPixel(localX, localY);
    writePixel(localX, localY, detail::blend565(destination, rgb565, alpha));
}

void Surface::blendPixel(int x, int y, uint16_t rgb565, uint8_t alpha) {
    if (alpha == 0 || !clip_.contains(x, y)) {
        return;
    }
    blendPixelRaw(x, y, rgb565, alpha);
    markDirtyRect(x, y, x + 1, y + 1);
}

} // namespace gfx
