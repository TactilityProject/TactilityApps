// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Font.h"
#include "Rect.h"

#include <graphics/pixel_buffer.h>
#include <tactility/drivers/display.h>

#include <cstddef>
#include <cstdint>

namespace gfx {

class DirtyRegions;

/**
 * The drawable slab a render callback receives, or an off-screen buffer an app draws into.
 *
 * Coordinates are world coordinates in the display's own pixel space: x in [0, width()) and y in
 * [originY(), originY() + height()). In banded mode the surface covers one horizontal band only, so
 * anything drawn outside it is clipped. A render callback can therefore draw the whole scene
 * without knowing which render mode the target picked.
 *
 * Buffers start black. In banded mode the target clears each band before the callback runs. In the
 * flip and full-frame modes the buffer persists between frames, so a callback has to clear a region
 * it drew into before drawing it again.
 *
 * Colour arguments are RGB565. Textures are written in the display's native format.
 *
 * A Surface never owns its buffer. Construct one from an existing PixelBuffer to draw off-screen:
 *
 *     PixelBuffer* buffer = pixel_buffer_create(DISPLAY_COLOR_FORMAT_RGB565, 128, 64);
 *     gfx::Surface surface(buffer);
 *     surface.fillRect(0, 0, 128, 64, 0xF800);
 *     pixel_buffer_free(buffer);
 */
class Surface {
public:
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    /** Wraps a whole buffer. Drawing coordinates start at the buffer's top-left. */
    explicit Surface(struct PixelBuffer* buffer);

    /** Wraps a sub-rectangle of a larger buffer. Drawing coordinates start at the view's top-left. */
    Surface(struct PixelBuffer* buffer, const Rect& view);

    int width() const { return bounds_.width(); }

    /** World rows this surface covers: the band height in banded mode, the display height otherwise. */
    int height() const { return bounds_.height(); }

    /** World x of this surface's first column. */
    int originX() const { return bounds_.x0; }

    /** World y of this surface's first row. */
    int originY() const { return bounds_.y0; }

    /** The world rectangle this surface covers. */
    const Rect& bounds() const { return bounds_; }

    /** The current clip, always inside bounds(). */
    const Rect& clip() const { return clip_; }

    /** Restricts drawing to `clip`, intersected with bounds(). */
    void setClip(const Rect& clip);

    /** Restores the clip to the whole surface. */
    void resetClip() { clip_ = bounds_; }

    enum DisplayColorFormat format() const { return format_; }

    /** True when the surface was constructed with a usable buffer. */
    bool valid() const { return buffer_ != nullptr && data_ != nullptr && !bounds_.empty(); }

    /** The underlying buffer, for handing to pixel_buffer_* calls. Never owned by the surface. */
    struct PixelBuffer* buffer() const { return buffer_; }

    /** Fills the whole surface with one colour (black by default). */
    void clear(uint16_t rgb565 = 0x0000);

    void fillRect(int x, int y, int width, int height, uint16_t rgb565);

    void fillRect(const Rect& rect, uint16_t rgb565) { fillRect(rect.x0, rect.y0, rect.width(), rect.height(), rgb565); }

    /** Horizontal line, `length` pixels starting at (x, y). */
    void hline(int x, int y, int length, uint16_t rgb565);

    /** Vertical line, `length` pixels starting at (x, y). */
    void vline(int x, int y, int length, uint16_t rgb565);

    /** Arbitrary line (Bresenham). */
    void line(int x0, int y0, int x1, int y1, uint16_t rgb565);

    /** Rectangle outline; `width` and `height` are the outer size. */
    void drawRect(int x, int y, int width, int height, uint16_t rgb565);

    void drawRect(const Rect& rect, uint16_t rgb565) { drawRect(rect.x0, rect.y0, rect.width(), rect.height(), rgb565); }

    /** Filled / outline circle (midpoint algorithm). */
    void fillCircle(int cx, int cy, int radius, uint16_t rgb565);

    void drawCircle(int cx, int cy, int radius, uint16_t rgb565);

    /** Filled / outline axis-aligned ellipse. */
    void fillEllipse(int cx, int cy, int radiusX, int radiusY, uint16_t rgb565);

    void drawEllipse(int cx, int cy, int radiusX, int radiusY, uint16_t rgb565);

    /** Rounded rectangle, filled / outline. `radius` is clamped to half the shorter side. */
    void fillRoundRect(int x, int y, int width, int height, int radius, uint16_t rgb565);

    void drawRoundRect(int x, int y, int width, int height, int radius, uint16_t rgb565);

    /** Triangle, filled / outline. */
    void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t rgb565);

    void drawTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t rgb565);

    /**
     * Filled / outline arc or annulus. Angles are in degrees: 0 is right, 90 is down, 180 is left,
     * 270 is up. `outerRadius` and `innerRadius` describe the ring; pass 0 for a solid wedge.
     */
    void fillArc(int cx, int cy, int outerRadius, int innerRadius, float startDegrees, float endDegrees, uint16_t rgb565);

    void drawArc(int cx, int cy, int outerRadius, int innerRadius, float startDegrees, float endDegrees, uint16_t rgb565);

    /** Fills a rectangle with a linear RGB565 gradient. */
    void gradient(const Rect& rect, uint16_t from, uint16_t to, bool horizontal = false);

    // -----------------------------------------------------------------------
    // Images and blending
    // -----------------------------------------------------------------------

    /**
     * Copies a region from another buffer, converting formats as needed. Source and destination
     * rectangles are clipped to the source buffer and this surface's clip. The source buffer must
     * not overlap this surface.
     */
    void blit(const struct PixelBuffer& source, int sourceX, int sourceY, int width, int height, int destX, int destY, enum PixelBufferConversion conversion = PIXEL_BUFFER_CONVERSION_EXACT_BLACK);

    /**
     * Like blit(), but skips pixels equal to transparentRgb565 (a colour key). The source buffer
     * must not overlap this surface.
     */
    void blitTransparent(const struct PixelBuffer& source, int sourceX, int sourceY, int width, int height, int destX, int destY, uint16_t transparentRgb565, enum PixelBufferConversion conversion = PIXEL_BUFFER_CONVERSION_EXACT_BLACK);

    /**
     * Draws a 1bpp icon mask in one colour. `maskStride` is bytes per mask row (0 means the tight
     * default). `msbFirst` selects bit 7 (true) or bit 0 (false) as the leftmost pixel.
     */
    void drawMask(const uint8_t* mask, int maskStride, int width, int height, int x, int y, uint16_t rgb565, bool msbFirst = true);

    /**
     * Draws an 8bpp alpha mask, blending `rgb565` into the destination. `alphaStride` is bytes per
     * mask row (0 means `width`).
     */
    void drawAlphaMask(const uint8_t* alpha, int alphaStride, int width, int height, int x, int y, uint16_t rgb565);

    /** Blends one pixel over the existing destination colour. alpha is 0..255. */
    void blendPixel(int x, int y, uint16_t rgb565, uint8_t alpha);

    // -----------------------------------------------------------------------
    // Polygons
    // -----------------------------------------------------------------------

    /** Polygon outline. */
    void drawPolygon(const Point* points, int count, uint16_t rgb565);

    /** Even-odd polygon fill. At most MAX_POLYGON_POINTS vertices are supported. */
    void fillPolygon(const Point* points, int count, uint16_t rgb565);

    static constexpr int MAX_POLYGON_POINTS = 32;

    void pixel(int x, int y, uint16_t rgb565);

    /**
     * Marks a world-space region as changed, for targets that present per region. Primitive calls
     * already do this; call it directly after writing through a raw pointer.
     */
    void markDirty(int x, int y, int width, int height);

    void markDirty(const Rect& rect) { markDirty(rect.x0, rect.y0, rect.width(), rect.height()); }

    // -----------------------------------------------------------------------
    // Text
    // -----------------------------------------------------------------------

    /** Draws transparent text: only glyph pixels are written. */
    void drawText(int x, int y, const char* text, const Font& font, uint16_t foreground);

    /** Draws text with an opaque background covering each glyph cell. */
    void drawText(int x, int y, const char* text, const Font& font, uint16_t foreground, uint16_t background);

    /** Draws text with an opaque background at an integer scale. */
    void drawText(int x, int y, const char* text, const Font& font, uint16_t foreground, uint16_t background, uint8_t scale);

    /**
     * Draws text using a style. For Left/Top alignment `x`/`y` are the top-left of the block; for
     * Center/Right and Middle/Bottom they are the anchor the block is aligned against.
     */
    void drawText(int x, int y, const char* text, const TextStyle& style);

    /**
     * Draws text inside a box, honouring the style's alignment and wrapping. Text that does not fit
     * vertically is clipped.
     */
    void drawText(const Rect& box, const char* text, const TextStyle& style);

private:
    friend class Target;
    friend class SinkTarget;

    /** Internal: a buffer whose (0,0) maps to world (0, originY). Used for bands. */
    Surface(struct PixelBuffer* buffer, int originY, int rows, DirtyRegions* dirty);

    void initialise(struct PixelBuffer* buffer, const Rect& bounds, uint8_t* data, DirtyRegions* dirty);

    void writePixel(int x, int localY, uint16_t rgb565);

    /** Write with an explicit mono/grayscale conversion policy. */
    void writePixel(int x, int localY, uint16_t rgb565, enum PixelBufferConversion conversion);

    /** Reads a pixel back as RGB565. x/localY are surface-local coordinates. */
    uint16_t readPixel(int x, int localY) const;

    /** Blends into a pixel without touching the dirty set. World coordinates. */
    void blendPixelRaw(int x, int y, uint16_t rgb565, uint8_t alpha);

    bool isFastRgb565() const;

    uint16_t toNative565(uint16_t rgb565) const;

    void markDirtyRect(int x0, int y0, int x1, int y1);

    /** Clipped write with no dirty marking. */
    void plotRaw(int x, int y, uint16_t rgb565);

    /** Clipped horizontal span with no dirty marking; x1 is exclusive. */
    void fillSpanRaw(int x0, int x1, int y, uint16_t rgb565);

    /** Clipped rectangle with no dirty marking. */
    void fillRectRaw(const Rect& rect, uint16_t rgb565);

    /** Clipped line with no dirty marking. */
    void lineRaw(int x0, int y0, int x1, int y1, uint16_t rgb565);

    /** Draws one glyph without touching the dirty set. */
    void drawGlyph(int x, int y, char c, const Font& font, uint16_t foreground, uint16_t background, bool opaque, uint8_t scale);

    /** Draws one line of text without touching the dirty set. */
    void drawTextLine(int x, int y, const char* begin, const char* end, const TextStyle& style);

    struct PixelBuffer* buffer_ = nullptr;
    uint8_t* data_ = nullptr;
    std::size_t stride_ = 0;
    Rect bounds_ {};
    Rect clip_ {};
    enum DisplayColorFormat format_ = DISPLAY_COLOR_FORMAT_RGB565;
    DirtyRegions* dirty_ = nullptr;
    /** Bit offset into the first byte of a monochrome sub-view. Zero for every other case. */
    int x_offset_bits_ = 0;
};

} // namespace gfx
