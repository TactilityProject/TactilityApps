// SPDX-License-Identifier: Apache-2.0
#include <gfx/Surface.h>

#include "ColorUtils.h"
#include "gfx/DirtyRegions.h"

#include <algorithm>
#include <cstdlib>

namespace gfx {

Surface::Surface(struct PixelBuffer* buffer) {
    if (buffer == nullptr) {
        return;
    }

    const int width = pixel_buffer_get_width(buffer);
    const int height = pixel_buffer_get_height(buffer);
    initialise(buffer, Rect { 0, 0, width, height }, static_cast<uint8_t*>(pixel_buffer_get_data(buffer)), nullptr);
}

Surface::Surface(struct PixelBuffer* buffer, const Rect& view) {
    if (buffer == nullptr) {
        return;
    }

    const auto format = pixel_buffer_get_format(buffer);
    const int buffer_width = pixel_buffer_get_width(buffer);
    const int buffer_height = pixel_buffer_get_height(buffer);
    const Rect clipped = view.intersect(Rect { 0, 0, buffer_width, buffer_height });
    if (clipped.empty()) {
        return;
    }

    const std::size_t stride = pixel_buffer_get_stride_bytes(buffer);
    const std::size_t x_offset = pixel_buffer_row_stride_bytes(format, clipped.x0);
    auto* data = static_cast<uint8_t*>(pixel_buffer_get_data(buffer)) + static_cast<std::size_t>(clipped.y0) * stride + x_offset;

    initialise(buffer, Rect { 0, 0, clipped.width(), clipped.height() }, data, nullptr);
    if (format == DISPLAY_COLOR_FORMAT_MONOCHROME) {
        // A sub-view can start inside a byte. writePixel() shifts by this amount for monochrome.
        x_offset_bits_ = clipped.x0 & 7;
    }
}

Surface::Surface(struct PixelBuffer* buffer, int originY, int rows, DirtyRegions* dirty) {
    if (buffer == nullptr) {
        return;
    }

    const int width = pixel_buffer_get_width(buffer);
    initialise(buffer, Rect { 0, originY, width, originY + rows }, static_cast<uint8_t*>(pixel_buffer_get_data(buffer)), dirty);
}

void Surface::initialise(struct PixelBuffer* buffer, const Rect& bounds, uint8_t* data, DirtyRegions* dirty) {
    buffer_ = buffer;
    data_ = data;
    stride_ = pixel_buffer_get_stride_bytes(buffer);
    bounds_ = bounds;
    clip_ = bounds;
    format_ = pixel_buffer_get_format(buffer);
    dirty_ = dirty;
}

void Surface::setClip(const Rect& clip) {
    clip_ = clip.intersect(bounds_);
}

bool Surface::isFastRgb565() const {
    switch (format_) {
        case DISPLAY_COLOR_FORMAT_RGB565:
        case DISPLAY_COLOR_FORMAT_RGB565_SWAPPED:
        case DISPLAY_COLOR_FORMAT_BGR565:
        case DISPLAY_COLOR_FORMAT_BGR565_SWAPPED:
            return true;
        default:
            return false;
    }
}

uint16_t Surface::toNative565(uint16_t rgb565) const {
    return detail::encode565(format_, rgb565);
}

void Surface::writePixel(int x, int local_y, uint16_t rgb565) {
    writePixel(x, local_y, rgb565, PIXEL_BUFFER_CONVERSION_EXACT_BLACK);
}

void Surface::writePixel(int x, int local_y, uint16_t rgb565, enum PixelBufferConversion conversion) {
    uint8_t* row = data_ + static_cast<std::size_t>(local_y) * stride_;
    switch (format_) {
        case DISPLAY_COLOR_FORMAT_RGB565:
            reinterpret_cast<uint16_t*>(row)[x] = rgb565;
            break;
        case DISPLAY_COLOR_FORMAT_RGB565_SWAPPED:
            reinterpret_cast<uint16_t*>(row)[x] = detail::byteswap16(rgb565);
            break;
        case DISPLAY_COLOR_FORMAT_BGR565:
            reinterpret_cast<uint16_t*>(row)[x] = detail::swapRedBlue(rgb565);
            break;
        case DISPLAY_COLOR_FORMAT_BGR565_SWAPPED:
            reinterpret_cast<uint16_t*>(row)[x] = detail::byteswap16(detail::swapRedBlue(rgb565));
            break;
        case DISPLAY_COLOR_FORMAT_RGB888: {
            uint8_t rgb[3];
            detail::toRgb888(rgb565, rgb);
            row[x * 3] = rgb[0];
            row[x * 3 + 1] = rgb[1];
            row[x * 3 + 2] = rgb[2];
            break;
        }
        case DISPLAY_COLOR_FORMAT_GRAYSCALE8:
            row[x] = detail::toGrayscale(rgb565, conversion);
            break;
        case DISPLAY_COLOR_FORMAT_MONOCHROME:
        default: {
            const int shifted = x + x_offset_bits_;
            const uint8_t mask = static_cast<uint8_t>(0x80U >> (shifted & 7));
            if (detail::isInk(rgb565, conversion)) {
                row[shifted >> 3] |= mask;
            } else {
                row[shifted >> 3] &= static_cast<uint8_t>(~mask);
            }
            break;
        }
    }
}

uint16_t Surface::readPixel(int x, int local_y) const {
    const uint8_t* row = data_ + static_cast<std::size_t>(local_y) * stride_;
    switch (format_) {
        case DISPLAY_COLOR_FORMAT_RGB565:
            return reinterpret_cast<const uint16_t*>(row)[x];
        case DISPLAY_COLOR_FORMAT_RGB565_SWAPPED:
            return detail::byteswap16(reinterpret_cast<const uint16_t*>(row)[x]);
        case DISPLAY_COLOR_FORMAT_BGR565:
            return detail::swapRedBlue(reinterpret_cast<const uint16_t*>(row)[x]);
        case DISPLAY_COLOR_FORMAT_BGR565_SWAPPED:
            return detail::swapRedBlue(detail::byteswap16(reinterpret_cast<const uint16_t*>(row)[x]));
        case DISPLAY_COLOR_FORMAT_RGB888: {
            const uint8_t rgb[3] = { row[x * 3], row[x * 3 + 1], row[x * 3 + 2] };
            return detail::rgb888To565(rgb);
        }
        case DISPLAY_COLOR_FORMAT_GRAYSCALE8: {
            const uint8_t gray = row[x];
            const uint8_t rgb[3] = { gray, gray, gray };
            return detail::rgb888To565(rgb);
        }
        case DISPLAY_COLOR_FORMAT_MONOCHROME:
        default: {
            const int shifted = x + x_offset_bits_;
            const uint8_t mask = static_cast<uint8_t>(0x80U >> (shifted & 7));
            return (row[shifted >> 3] & mask) ? 0xFFFF : 0x0000;
        }
    }
}

void Surface::markDirtyRect(int x0, int y0, int x1, int y1) {
    if (dirty_ == nullptr) {
        return;
    }

    x0 = std::max(x0, clip_.x0);
    x1 = std::min(x1, clip_.x1);
    y0 = std::max(y0, clip_.y0);
    y1 = std::min(y1, clip_.y1);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    dirty_->add(Rect { x0, y0, x1, y1 });
}

void Surface::markDirty(int x, int y, int width, int height) {
    markDirtyRect(x, y, x + width, y + height);
}

void Surface::plotRaw(int x, int y, uint16_t rgb565) {
    if (!clip_.contains(x, y)) {
        return;
    }
    writePixel(x - bounds_.x0, y - bounds_.y0, rgb565);
}

void Surface::fillSpanRaw(int x0, int x1, int y, uint16_t rgb565) {
    x0 = std::max(x0, clip_.x0);
    x1 = std::min(x1, clip_.x1);
    if (x0 >= x1 || y < clip_.y0 || y >= clip_.y1) {
        return;
    }

    const int local_x0 = x0 - bounds_.x0;
    const int local_x1 = x1 - bounds_.x0;
    const int local_y = y - bounds_.y0;
    uint8_t* row = data_ + static_cast<std::size_t>(local_y) * stride_;

    if (isFastRgb565()) {
        const uint16_t native = toNative565(rgb565);
        auto* pixels = reinterpret_cast<uint16_t*>(row);
        std::fill(pixels + local_x0, pixels + local_x1, native);
        return;
    }

    if (format_ == DISPLAY_COLOR_FORMAT_GRAYSCALE8) {
        std::fill(row + local_x0, row + local_x1, detail::toLuma(rgb565));
        return;
    }

    for (int x = local_x0; x < local_x1; x++) {
        writePixel(x, local_y, rgb565);
    }
}

void Surface::fillRectRaw(const Rect& rect, uint16_t rgb565) {
    const Rect clipped = rect.intersect(clip_);
    if (clipped.empty()) {
        return;
    }

    for (int y = clipped.y0; y < clipped.y1; y++) {
        fillSpanRaw(clipped.x0, clipped.x1, y, rgb565);
    }
}

void Surface::fillRect(int x, int y, int width, int height, uint16_t rgb565) {
    if (width <= 0 || height <= 0) {
        return;
    }

    const Rect clipped = Rect { x, y, x + width, y + height }.intersect(clip_);
    if (clipped.empty()) {
        return;
    }

    fillRectRaw(clipped, rgb565);
    markDirtyRect(clipped.x0, clipped.y0, clipped.x1, clipped.y1);
}

void Surface::hline(int x, int y, int length, uint16_t rgb565) {
    fillRect(x, y, length, 1, rgb565);
}

void Surface::vline(int x, int y, int length, uint16_t rgb565) {
    fillRect(x, y, 1, length, rgb565);
}

void Surface::pixel(int x, int y, uint16_t rgb565) {
    if (!clip_.contains(x, y)) {
        return;
    }
    writePixel(x - bounds_.x0, y - bounds_.y0, rgb565);
    markDirtyRect(x, y, x + 1, y + 1);
}

void Surface::clear(uint16_t rgb565) {
    fillRect(bounds_, rgb565);
}

void Surface::lineRaw(int x0, int y0, int x1, int y1, uint16_t rgb565) {
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;

    while (true) {
        plotRaw(x0, y0, rgb565);
        if (x0 == x1 && y0 == y1) {
            break;
        }

        const int e2 = 2 * error;
        if (e2 >= dy) {
            error += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

void Surface::line(int x0, int y0, int x1, int y1, uint16_t rgb565) {
    const Rect box = Rect {
        std::min(x0, x1),
        std::min(y0, y1),
        std::max(x0, x1) + 1,
        std::max(y0, y1) + 1,
    }.intersect(clip_);
    if (box.empty()) {
        return;
    }

    markDirtyRect(box.x0, box.y0, box.x1, box.y1);
    lineRaw(x0, y0, x1, y1, rgb565);
}

void Surface::drawRect(int x, int y, int width, int height, uint16_t rgb565) {
    if (width <= 0 || height <= 0) {
        return;
    }

    const Rect box = Rect { x, y, x + width, y + height }.intersect(clip_);
    if (box.empty()) {
        return;
    }

    markDirtyRect(box.x0, box.y0, box.x1, box.y1);

    fillSpanRaw(x, x + width, y, rgb565);
    if (height > 1) {
        fillSpanRaw(x, x + width, y + height - 1, rgb565);
    }
    if (height > 2) {
        fillRectRaw(Rect { x, y + 1, x + 1, y + height - 1 }, rgb565);
        fillRectRaw(Rect { x + width - 1, y + 1, x + width, y + height - 1 }, rgb565);
    }
}

void Surface::gradient(const Rect& rect, uint16_t from, uint16_t to, bool horizontal) {
    const Rect area = rect.intersect(clip_);
    if (area.empty()) {
        return;
    }

    markDirtyRect(area.x0, area.y0, area.x1, area.y1);

    if (horizontal) {
        const int width = rect.width();
        for (int x = area.x0; x < area.x1; x++) {
            const int amount = width <= 1 ? 0 : (x - rect.x0) * 255 / (width - 1);
            fillRectRaw(Rect { x, area.y0, x + 1, area.y1 }, detail::blend565(from, to, static_cast<uint8_t>(amount)));
        }
        return;
    }

    const int height = rect.height();
    for (int y = area.y0; y < area.y1; y++) {
        const int amount = height <= 1 ? 0 : (y - rect.y0) * 255 / (height - 1);
        fillSpanRaw(area.x0, area.x1, y, detail::blend565(from, to, static_cast<uint8_t>(amount)));
    }
}

} // namespace gfx
