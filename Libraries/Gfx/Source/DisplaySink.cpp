// SPDX-License-Identifier: Apache-2.0
#include <gfx/DisplaySink.h>

#include <tactility/device.h>
#include <tactility/drivers/display.h>

namespace gfx {

DisplaySink::DisplaySink(struct Device* display) : display_(display) {
    if (display_ != nullptr && device_get(display_) != ERROR_NONE) {
        display_ = nullptr;
    }
}

DisplaySink::~DisplaySink() {
    if (display_ != nullptr) {
        device_put(display_);
    }
}

bool DisplaySink::begin(const SinkInfo& info) {
    if (display_ == nullptr || info.width <= 0 || info.height <= 0) {
        return false;
    }
    if (info.format != display_get_color_format(display_)) {
        return false;
    }
    if (info.width != display_get_resolution_x(display_) || info.height != display_get_resolution_y(display_)) {
        return false;
    }
    info_ = info;
    begun_ = true;
    return true;
}

bool DisplaySink::present(const struct PixelBuffer& buffer, const Rect& rect) {
    if (!begun_) {
        return false;
    }

    const Rect clipped = rect.intersect(Rect { 0, 0, info_.width, info_.height });
    if (clipped.empty()) {
        return true;
    }

    auto* mutable_buffer = const_cast<struct PixelBuffer*>(&buffer);
    // draw_bitmap() has no stride parameter, so present whole rows. This works for every format,
    // including monochrome, and keeps a sub-rectangle from leaking into the next row.
    pixel_buffer_msync(mutable_buffer, 0, clipped.y0, info_.width, clipped.height());
    return display_draw_bitmap(
        display_,
        0,
        clipped.y0,
        info_.width,
        clipped.y1,
        pixel_buffer_get_row(mutable_buffer, clipped.y0)
    ) == ERROR_NONE;
}

void DisplaySink::end() {
    begun_ = false;
}

bool DisplaySink::frame_buffer_policy(MemoryPolicy& out) const {
    if (display_ == nullptr) {
        return false;
    }
    // If the driver copies out of the buffer, external RAM is fine. Otherwise the driver may DMA
    // straight from it, which PSRAM can't do on ESP32-class chips.
    if (display_has_capability(display_, DISPLAY_CAPABILITY_PREFER_EXTERNAL_RAM)) {
        return false;
    }
    out = MemoryPolicy { MEMORY_CAPABILITY_DMA, MEMORY_CAPABILITY_EXTERNAL, 64 };
    return true;
}

error_t DisplaySink::refresh() {
    if (display_ == nullptr) {
        return ERROR_INVALID_STATE;
    }
    return display_refresh(display_);
}

} // namespace gfx
