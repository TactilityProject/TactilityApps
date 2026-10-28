// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Sink.h"

namespace gfx {

/**
 * A Sink that presents to a Tactility display Device with display_draw_bitmap().
 *
 * This is the generic full-frame path: it presents whole rows, so it works with every display
 * colour format, including monochrome. For flip/full-frame/banded policy and hardware
 * rotation/scaling, use gfx::Target instead.
 */
class DisplaySink : public Sink {
public:
    /** Takes a reference on the device until destruction. */
    explicit DisplaySink(struct Device* display);

    ~DisplaySink() override;

    DisplaySink(const DisplaySink&) = delete;
    DisplaySink& operator=(const DisplaySink&) = delete;

    bool begin(const SinkInfo& info) override;

    bool present(const struct PixelBuffer& buffer, const Rect& rect) override;

    void end() override;

    bool frame_buffer_policy(MemoryPolicy& out) const override;

    error_t refresh() override;

private:
    struct Device* display_ = nullptr;
    SinkInfo info_ {};
    bool begun_ = false;
};

} // namespace gfx
