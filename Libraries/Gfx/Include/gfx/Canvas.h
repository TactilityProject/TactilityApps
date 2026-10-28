// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Surface.h"

#include <graphics/pixel_buffer.h>
#include <tactility/memory.h>

namespace gfx {

/**
 * An owned PixelBuffer plus a Surface over it.
 *
 * Useful for off-screen composition: draw a sprite once, then blit it into a Target surface.
 * The buffer is allocated with pixel_buffer_create() (external RAM preferred, 64-byte aligned)
 * unless an explicit MemoryPolicy is supplied. A Canvas never allocates in the drawing path.
 *
 *     gfx::Canvas sprite(32, 32, DISPLAY_COLOR_FORMAT_RGB565);
 *     if (sprite.valid()) {
 *         sprite.surface().clear(0xF81F);       // colour key
 *         sprite.surface().fillCircle(16, 16, 12, 0x07FF);
 *     }
 *     target.render([&](gfx::Surface& s) {
 *         s.blitTransparent(*sprite.buffer(), 0, 0, 32, 32, x, y, 0xF81F);
 *     });
 */
class Canvas {
public:
    Canvas(int width, int height, enum DisplayColorFormat format = DISPLAY_COLOR_FORMAT_RGB565, const MemoryPolicy* policy = nullptr);

    ~Canvas();

    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;
    Canvas(Canvas&&) = delete;
    Canvas& operator=(Canvas&&) = delete;

    bool valid() const { return surface_.valid(); }

    Surface& surface() { return surface_; }

    const Surface& surface() const { return surface_; }

    struct PixelBuffer* buffer() { return buffer_; }

    const struct PixelBuffer* buffer() const { return buffer_; }

    int width() const { return surface_.width(); }

    int height() const { return surface_.height(); }

    enum DisplayColorFormat format() const { return surface_.format(); }

    void clear(uint16_t rgb565 = 0x0000) { surface_.clear(rgb565); }

private:
    struct PixelBuffer* buffer_ = nullptr;
    Surface surface_;
};

} // namespace gfx
