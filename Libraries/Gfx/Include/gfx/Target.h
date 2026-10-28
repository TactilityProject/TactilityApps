// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DirtyRegions.h"
#include "Surface.h"

#include <type_traits>

namespace gfx {

/** How the render buffer is prepared for a frame. */
enum class Prepare {
    /**
     * Copy the previously displayed frame into the render buffer first, so the callback only has to
     * redraw the regions it changes. Safe default, but costs a copy of the changed regions.
     */
    Preserve,
    /**
     * Don't copy anything: the callback guarantees it overwrites every pixel that differs between
     * frames, including content left over from earlier frames. Lets an app that draws thin
     * primitives erase them by redrawing them in the background colour instead of clearing
     * bounding boxes, which is far cheaper.
     */
    Overwrite,
};

enum class RenderMode {
    /** Construction failed; the target can't render. */
    None,
    /** Two hardware frame buffers: the frame is drawn into the back one and presented by flipping. */
    Flip,
    /** One full-frame buffer; only the regions that changed are sent to the display. */
    FullFrame,
    /** A small band buffer, rendered and presented band by band. Uses the least memory. */
    Banded,
    /** A logical frame that the display's scaler/rotator maps onto the panel (see set_transform()). */
    Transform,
};

/** A logical frame size plus the rotation applied when putting it on the panel. */
struct Transform {
    /** Size the render callback draws into. */
    int width = 0;
    int height = 0;
    /** Degrees clockwise to rotate when presenting: 0, 90, 180 or 270. */
    int rotation = 0;
};

struct Info {
    int width = 0;
    int height = 0;
    enum DisplayColorFormat format = DISPLAY_COLOR_FORMAT_RGB565;
    RenderMode mode = RenderMode::None;

    /**
     * Size the render callback draws into: the native size unless set_transform() succeeded, in
     * which case it is the logical (possibly rotated/scaled) size.
     */
    int frameWidth = 0;
    int frameHeight = 0;

    /** World rows per band for RenderMode::Banded, the display height otherwise. */
    int bandHeight = 0;

    /** The panel is slow to refresh (e-paper): animating it makes no sense. */
    bool slowRefresh = false;

    /** The panel needs a whole frame per update, so per-region presenting is not possible. */
    bool fullFrameRequired = false;

    /** Frames are rendered into the panel's own frame buffer(s), not into a scratch buffer. */
    bool hardwareFrameBuffer = false;
};

/** What the last rendered frame actually cost. Useful for tuning on real hardware. */
struct Stats {
    /** Rectangles the frame touched (1 for a whole-frame present). */
    int dirtyRects = 0;
    /** Pixels covered by those rectangles. */
    int dirtyPixels = 0;
    /** Microseconds per phase of the last frame (0 on platforms without a timer). */
    int copyUs = 0;
    int drawUs = 0;
    int syncUs = 0;
    int presentUs = 0;
    int totalUs = 0;
};

/**
 * A display configured for direct rendering, outside LVGL.
 *
 * The target picks how to get pixels onto the panel: flip between two hardware frame buffers (MIPI
 * DSI, RGB panels), present dirty regions from one full-frame buffer (panels with PSRAM to spare),
 * or render band by band from a small buffer (panels with little RAM). Callers only see a Surface.
 */
class Target {
public:
    /** Takes a reference on @a display and keeps it until destruction. */
    explicit Target(struct Device* display);

    ~Target();

    Target(const Target&) = delete;
    Target& operator=(const Target&) = delete;

    bool valid() const { return info_.mode != RenderMode::None; }

    /**
     * Frees all render buffers and releases the PPA client. The display keeps its last presented
     * frame. Intended for apps that use the primary display briefly and then need the memory for
     * another target; further render() calls do nothing.
     */
    void release();

    const Info& info() const { return info_; }

    const Stats& stats() const { return stats_; }

    /**
     * Runs the display's own refresh/maintenance pass, e.g. an e-paper panel's full-quality redraw
     * that resolves grayscale or clears ghosting. Presenting does a fast partial update, so apps
     * that draw static content on such a panel should call this once afterwards.
     * @retval ERROR_NOT_SUPPORTED when the display has no refresh pass
     */
    error_t refresh();

    /**
     * Draws into a logical frame and lets the display's scaler/rotator put it on the panel. Use it
     * to work in landscape coordinates on a portrait-native panel, or to render at a lower
     * resolution and let the hardware scale up.
     * @retval true  subsequent frames use the transform; info().frameWidth/frameHeight describe the
     *               surface the callback gets
     * @retval false unsupported (no hardware scaler/rotator, or a display format it can't handle),
     *               in which case the target keeps rendering at the native size and rotation
     */
    bool set_transform(const Transform& transform);

    /** Renders one frame. In banded mode the callback runs once per band with that band's surface. */
    using RenderCallback = void (*)(Surface& surface, void* context);

    void render(RenderCallback callback, void* context, Prepare prepare = Prepare::Preserve);

    template <typename Function>
    void render(Function&& function, Prepare prepare = Prepare::Preserve) {
        using Fn = std::remove_reference_t<Function>;
        Fn* fn = &function;
        render([](Surface& surface, void* context) {
            (*static_cast<Fn*>(context))(surface);
        }, fn, prepare);
    }

private:
    bool setup();

    bool setupFlip();

    bool setupFullFrame(bool required);

    bool setupBanded();

    Rect clampRect(const Rect& rect) const;

    void presentDirty(struct PixelBuffer* buffer);

    void copyRect(struct PixelBuffer* dst, struct PixelBuffer* src, const Rect& rect);

    void updateStats(bool whole_frame);

    void presentTransformed(struct PixelBuffer* output);

    struct Device* display_ = nullptr;
    Info info_ {};
    Stats stats_ {};
    struct PixelBuffer* flip_buffers_[2] = { nullptr, nullptr };
    struct PixelBuffer* frame_buffer_ = nullptr;
    struct PixelBuffer* band_buffer_ = nullptr;
    // set_transform(): logical frame, its scaler client and the native-size output when there are
    // no hardware frame buffers to write into
    struct PixelBuffer* source_buffer_ = nullptr;
    struct PixelBuffer* output_buffer_ = nullptr;
    void* ppa_client_ = nullptr;
    Transform transform_ {};
    bool using_hw_output_ = false;
    uint8_t* staging_ = nullptr;
    std::size_t staging_capacity_ = 0;
    int back_index_ = 0;
    bool present_full_ = false;
    DirtyRegions dirty_;
    // The regions the previous frame touched: the only ones that differ between the two flip
    // buffers, so only those have to be copied forward
    DirtyRegions previous_dirty_;
    bool flip_has_previous_ = false;
    bool first_frame_ = true;
};

} // namespace gfx
