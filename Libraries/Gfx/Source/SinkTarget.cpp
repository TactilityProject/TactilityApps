// SPDX-License-Identifier: Apache-2.0
#include <gfx/Sink.h>

#include <tactility/log.h>

#include <algorithm>

#ifdef ESP_PLATFORM
#include <esp_timer.h>
#endif

namespace gfx {

namespace {

constexpr auto* TAG = "gfx";

int64_t nowUs() {
#ifdef ESP_PLATFORM
    return esp_timer_get_time();
#else
    return 0;
#endif
}

} // namespace

SinkTarget::SinkTarget(Sink& sink, int width, int height, enum DisplayColorFormat format)
    : sink_(sink) {
    if (width <= 0 || height <= 0) {
        return;
    }

    frame_buffer_ = nullptr;

    MemoryPolicy policy {};
    if (sink_.frame_buffer_policy(policy)) {
        frame_buffer_ = pixel_buffer_create_with_policy(format, width, height, &policy);
    } else {
        frame_buffer_ = pixel_buffer_create(format, width, height);
    }
    if (frame_buffer_ == nullptr) {
        LOG_E(TAG, "Failed to allocate a %dx%d frame buffer for the sink", width, height);
        return;
    }

    const SinkInfo sink_info { width, height, format };
    if (!sink_.begin(sink_info)) {
        LOG_E(TAG, "Sink rejected a %dx%d format-%d frame", width, height, static_cast<int>(format));
        pixel_buffer_free(frame_buffer_);
        frame_buffer_ = nullptr;
        return;
    }

    begun_ = true;
    info_.width = width;
    info_.height = height;
    info_.format = format;
    info_.mode = RenderMode::FullFrame;
    info_.frameWidth = width;
    info_.frameHeight = height;
    info_.bandHeight = height;
    info_.slowRefresh = false;
    info_.fullFrameRequired = false;
    info_.hardwareFrameBuffer = false;
}

SinkTarget::~SinkTarget() {
    if (begun_) {
        sink_.end();
    }
    if (frame_buffer_ != nullptr) {
        pixel_buffer_free(frame_buffer_);
    }
}

void SinkTarget::render(RenderCallback callback, void* context, Prepare prepare) {
    (void)prepare;

    if (!valid() || callback == nullptr) {
        return;
    }

    const int64_t t0 = nowUs();

    dirty_.clear();
    Surface surface(frame_buffer_, 0, info_.height, &dirty_);
    callback(surface, context);

    const int64_t t1 = nowUs();

    auto present = [&](const Rect& rect) {
        const Rect clipped = rect.intersect(Rect { 0, 0, info_.width, info_.height });
        if (clipped.empty()) {
            return;
        }

        // Full rows are safe for every format (including monochrome), and sinks can use the
        // buffer's stride directly.
        pixel_buffer_msync(frame_buffer_, 0, clipped.y0, info_.width, clipped.height());
        if (!sink_.present(*frame_buffer_, clipped)) {
            LOG_W(TAG, "Sink rejected a %dx%d region at (%d, %d)", clipped.width(), clipped.height(), clipped.x0, clipped.y0);
        }
    };

    if (first_frame_) {
        present(Rect { 0, 0, info_.width, info_.height });
        first_frame_ = false;
        stats_.dirtyRects = 1;
        stats_.dirtyPixels = info_.width * info_.height;
    } else {
        stats_.dirtyRects = 0;
        stats_.dirtyPixels = 0;
        for (int i = 0; i < dirty_.count(); i++) {
            const Rect rect = dirty_.at(i).intersect(Rect { 0, 0, info_.width, info_.height });
            if (rect.empty()) {
                continue;
            }
            present(rect);
            stats_.dirtyRects++;
            stats_.dirtyPixels += rect.width() * rect.height();
        }
    }

    const int64_t t2 = nowUs();
    stats_.copyUs = 0;
    stats_.drawUs = static_cast<int>(t1 - t0);
    stats_.syncUs = 0;
    stats_.presentUs = static_cast<int>(t2 - t1);
    stats_.totalUs = static_cast<int>(t2 - t0);
}

} // namespace gfx
