// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DirtyRegions.h"
#include "Surface.h"
#include "Target.h"

#include <graphics/pixel_buffer.h>

#include <type_traits>

namespace gfx {

/** The frame properties a Sink is asked to accept. */
struct SinkInfo {
    int width = 0;
    int height = 0;
    enum DisplayColorFormat format = DISPLAY_COLOR_FORMAT_RGB565;
};

/**
 * A presentation backend for SinkTarget.
 *
 * A Sink owns no pixels: SinkTarget keeps the full-frame PixelBuffer and hands the sink the
 * rectangles that changed. present() may be called many times per frame. Returning false is
 * allowed when the backend cannot accept that region; SinkTarget logs it and carries on.
 *
 * Implementations should treat `buffer` as read-only and use `pixel_buffer_get_row()` for stride.
 */
class Sink {
public:
    virtual ~Sink() = default;

    /** Called once before the first frame. Return false when the backend is unavailable. */
    virtual bool begin(const SinkInfo& info) = 0;

    /** Presents one changed rectangle. `rect` is clamped to the buffer and is never empty. */
    virtual bool present(const struct PixelBuffer& buffer, const Rect& rect) = 0;

    /** Called once when the target is destroyed. */
    virtual void end() = 0;

    /**
     * Optional frame-buffer placement. Return true and fill `out` when the backend needs a
     * specific MemoryPolicy (e.g. a display driver that DMAs from the buffer). The default leaves
     * the buffer in external RAM when possible.
     */
    virtual bool frame_buffer_policy(MemoryPolicy& out) const {
        (void)out;
        return false;
    }

    /** Optional full-quality refresh, for e-paper-like backends. */
    virtual error_t refresh() { return ERROR_NOT_SUPPORTED; }
};

/**
 * A display-independent Target for a Sink.
 *
 * The target owns one full-frame PixelBuffer and presents changed rectangles to the sink. It is
 * the generic path for I2C LCD units, headless bitmaps, or any backend that is not a Tactility
 * display device. Use gfx::Target for displays: it adds frame-buffer flipping, banded rendering
 * and hardware rotation/scaling.
 *
 * The render buffer persists between frames, so Prepare is accepted for API symmetry but has no
 * effect. Callers that only draw changed pixels can leave them alone; callers that need a clean
 * slate should clear first.
 */
class SinkTarget {
public:
    /** Takes a reference-free handle on `sink`; the sink must outlive this target. */
    SinkTarget(Sink& sink, int width, int height, enum DisplayColorFormat format = DISPLAY_COLOR_FORMAT_RGB565);

    ~SinkTarget();

    SinkTarget(const SinkTarget&) = delete;
    SinkTarget& operator=(const SinkTarget&) = delete;

    bool valid() const { return begun_ && frame_buffer_ != nullptr; }

    const Info& info() const { return info_; }

    const Stats& stats() const { return stats_; }

    /** The full-frame buffer the render callback draws into. Never owned by the caller. */
    struct PixelBuffer* buffer() { return frame_buffer_; }

    const struct PixelBuffer* buffer() const { return frame_buffer_; }

    /** Present a whole frame and then visit the sink's optional maintenance pass. */
    error_t refresh() { return sink_.refresh(); }

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
    Sink& sink_;
    struct PixelBuffer* frame_buffer_ = nullptr;
    DirtyRegions dirty_;
    Info info_ {};
    Stats stats_ {};
    bool begun_ = false;
    bool first_frame_ = true;
};

} // namespace gfx
