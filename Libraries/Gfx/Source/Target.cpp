// SPDX-License-Identifier: Apache-2.0
#include <gfx/Target.h>

#include <tactility/device.h>
#include <tactility/error.h>
#include <tactility/log.h>
#include <tactility/memory.h>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <soc/soc_caps.h>
#endif

#if defined(ESP_PLATFORM) && defined(SOC_PPA_SUPPORTED) && SOC_PPA_SUPPORTED
#include <driver/ppa.h>
#define GFX_HAS_PPA 1
#else
#define GFX_HAS_PPA 0
#endif

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace gfx {

namespace {

constexpr auto* TAG = "gfx";

/** Scratch space for one band. Small enough for an ESP32 without PSRAM, big enough for few bands. */
constexpr std::size_t BAND_BUDGET_BYTES = 32 * 1024;

/** Only use a full-frame buffer when it leaves at least this much room for the rest of the app. */
constexpr std::size_t FULL_FRAME_HEADROOM = 2;

#ifdef ESP_PLATFORM
int64_t nowUs() {
    return esp_timer_get_time();
}
#else
int64_t nowUs() {
    return 0;
}
#endif

} // namespace

Target::Target(struct Device* display) : display_(display) {
    if (display_ == nullptr) {
        return;
    }

    if (device_get(display_) != ERROR_NONE) {
        display_ = nullptr;
        return;
    }

    if (!setup()) {
        LOG_E(TAG, "Display can't be used for direct rendering");
    }
}

Target::~Target() {
    release();
    if (display_ != nullptr) {
        device_put(display_);
    }
}

void Target::release() {
    if (flip_buffers_[0] != nullptr) {
        pixel_buffer_free(flip_buffers_[0]);
        flip_buffers_[0] = nullptr;
    }
    if (flip_buffers_[1] != nullptr) {
        pixel_buffer_free(flip_buffers_[1]);
        flip_buffers_[1] = nullptr;
    }
    if (frame_buffer_ != nullptr) {
        pixel_buffer_free(frame_buffer_);
        frame_buffer_ = nullptr;
    }
    if (band_buffer_ != nullptr) {
        pixel_buffer_free(band_buffer_);
        band_buffer_ = nullptr;
    }
    if (staging_ != nullptr) {
        memory_free(staging_);
        staging_ = nullptr;
        staging_capacity_ = 0;
    }
#if GFX_HAS_PPA
    if (ppa_client_ != nullptr) {
        ppa_unregister_client(static_cast<ppa_client_handle_t>(ppa_client_));
        ppa_client_ = nullptr;
    }
#endif
    if (source_buffer_ != nullptr) {
        pixel_buffer_free(source_buffer_);
        source_buffer_ = nullptr;
    }
    if (output_buffer_ != nullptr) {
        pixel_buffer_free(output_buffer_);
        output_buffer_ = nullptr;
    }

    using_hw_output_ = false;
    back_index_ = 0;
    present_full_ = false;
    transform_ = {};
    dirty_.clear();
    previous_dirty_.clear();
    flip_has_previous_ = false;
    first_frame_ = true;
    info_.mode = RenderMode::None;
    info_.hardwareFrameBuffer = false;
}

bool Target::setup() {
    info_.width = display_get_resolution_x(display_);
    info_.height = display_get_resolution_y(display_);
    info_.format = display_get_color_format(display_);
    info_.slowRefresh = display_has_capability(display_, DISPLAY_CAPABILITY_SLOW_REFRESH);
    info_.fullFrameRequired = info_.format == DISPLAY_COLOR_FORMAT_MONOCHROME
        || display_has_capability(display_, DISPLAY_CAPABILITY_REQUIRES_FULL_FRAME);
    info_.bandHeight = info_.height;

    if (info_.width <= 0 || info_.height <= 0) {
        return false;
    }

    info_.frameWidth = info_.width;
    info_.frameHeight = info_.height;

    if (display_get_frame_buffer_count(display_) >= 2 && setupFlip()) {
        return true;
    }

    if (info_.fullFrameRequired) {
        return setupFullFrame(true);
    }

    if (setupFullFrame(false)) {
        return true;
    }

    return setupBanded();
}

bool Target::setupFlip() {
    void* first = nullptr;
    void* second = nullptr;
    display_get_frame_buffer(display_, 0, &first);
    display_get_frame_buffer(display_, 1, &second);
    if (first == nullptr || second == nullptr) {
        return false;
    }

    flip_buffers_[0] = pixel_buffer_wrap(first, info_.format, info_.width, info_.height, 0);
    flip_buffers_[1] = pixel_buffer_wrap(second, info_.format, info_.width, info_.height, 0);
    if (flip_buffers_[0] == nullptr || flip_buffers_[1] == nullptr) {
        return false;
    }

    // The buffers hold whatever the previous owner left behind, and a callback only clears the
    // regions it draws into, so start both from black.
    for (auto* buffer : flip_buffers_) {
        pixel_buffer_clear(buffer);
        pixel_buffer_msync(buffer, 0, 0, info_.width, info_.height);
    }

    back_index_ = 0;
    info_.hardwareFrameBuffer = true;
    info_.mode = RenderMode::Flip;
    LOG_I(TAG, "Rendering %dx%d by flipping hardware frame buffers", info_.width, info_.height);
    return true;
}

bool Target::setupFullFrame(bool required) {
    const std::size_t stride = pixel_buffer_row_stride_bytes(info_.format, info_.width);
    const std::size_t frame_bytes = stride * static_cast<std::size_t>(info_.height);
    const bool display_copies = display_has_capability(display_, DISPLAY_CAPABILITY_PREFER_EXTERNAL_RAM);

#ifdef ESP_PLATFORM
    if (!required) {
        // malloc-cap failures are the real test, but this keeps a frame from eating the whole heap
        const uint32_t caps = display_copies ? MALLOC_CAP_DEFAULT : MALLOC_CAP_DMA;
        if (frame_bytes * FULL_FRAME_HEADROOM > heap_caps_get_largest_free_block(caps)) {
            return false;
        }
    }
#else
    (void)required;
    (void)frame_bytes;
#endif

    // DMA-capable unless the driver copies out of the buffer, since panel drivers read it directly
    const MemoryPolicy policy = display_copies
        ? MemoryPolicy { 0, MEMORY_CAPABILITY_EXTERNAL, 64 }
        : MemoryPolicy { MEMORY_CAPABILITY_DMA, MEMORY_CAPABILITY_EXTERNAL, 64 };

    frame_buffer_ = pixel_buffer_create_with_policy(info_.format, info_.width, info_.height, &policy);
    if (frame_buffer_ == nullptr) {
        return false;
    }

    // Dirty rectangles are presented tightly packed (see presentDirty()), which needs a staging
    // buffer. Small panels' drivers copy the data anyway, so this is only CPU work there; for
    // panels that drive only the region they're given, a tight rectangle is much cheaper than
    // whole rows.
    if (!info_.fullFrameRequired) {
        staging_capacity_ = BAND_BUDGET_BYTES;
        const MemoryPolicy staging_policy { MEMORY_CAPABILITY_INTERNAL | MEMORY_CAPABILITY_DMA, 0, 64 };
        staging_ = static_cast<uint8_t*>(memory_alloc_with_policy(staging_capacity_, &staging_policy));
        if (staging_ == nullptr) {
            staging_capacity_ = 0;
        }
    }

    present_full_ = info_.fullFrameRequired;
    info_.mode = RenderMode::FullFrame;
    LOG_I(TAG, "Rendering %dx%d from a full-frame buffer", info_.width, info_.height);
    return true;
}

bool Target::setupBanded() {
    const std::size_t stride = pixel_buffer_row_stride_bytes(info_.format, info_.width);
    if (stride == 0) {
        return false;
    }

    int rows = static_cast<int>(BAND_BUDGET_BYTES / stride);
    rows = std::max(rows, 1);
    rows = std::min(rows, info_.height);

    // Spread the rows over the bands evenly, so the last band isn't a sliver
    const int bands = (info_.height + rows - 1) / rows;
    rows = (info_.height + bands - 1) / bands;

    // Bands are small and go straight to DMA, so keep them in internal memory: on ESP32-class
    // chips PSRAM can't be used for SPI DMA at all.
    const MemoryPolicy policy { MEMORY_CAPABILITY_INTERNAL | MEMORY_CAPABILITY_DMA, 0, 64 };
    band_buffer_ = pixel_buffer_create_with_policy(info_.format, info_.width, rows, &policy);
    if (band_buffer_ == nullptr) {
        // Last resort: some chips can DMA from external memory and have little internal RAM free
        const MemoryPolicy external { MEMORY_CAPABILITY_DMA, MEMORY_CAPABILITY_EXTERNAL, 64 };
        band_buffer_ = pixel_buffer_create_with_policy(info_.format, info_.width, rows, &external);
    }
    if (band_buffer_ == nullptr) {
        return false;
    }

    info_.bandHeight = rows;
    info_.mode = RenderMode::Banded;
    LOG_I(TAG, "Rendering %dx%d in %d bands of %d rows", info_.width, info_.height, bands, rows);
    return true;
}

Rect Target::clampRect(const Rect& rect) const {
    Rect result = rect;
    result.x0 = std::max(result.x0, 0);
    result.y0 = std::max(result.y0, 0);
    result.x1 = std::min(result.x1, info_.width);
    result.y1 = std::min(result.y1, info_.height);
    return result;
}

void Target::copyRect(struct PixelBuffer* dst, struct PixelBuffer* src, const Rect& rect) {
    if (info_.format == DISPLAY_COLOR_FORMAT_MONOCHROME) {
        // Bit-packed rows: let the module deal with the packing
        pixel_buffer_blit(
            dst, rect.x0, rect.y0,
            src, rect.x0, rect.y0,
            rect.width(), rect.height(),
            PIXEL_BUFFER_CONVERSION_EXACT_BLACK
        );
        return;
    }

    const std::size_t bytes_per_pixel = pixel_buffer_row_stride_bytes(info_.format, 1);
    const std::size_t stride = pixel_buffer_get_stride_bytes(src);
    const auto* src_data = static_cast<const uint8_t*>(pixel_buffer_get_data(src));
    auto* dst_data = static_cast<uint8_t*>(pixel_buffer_get_data(dst));

    const std::size_t row_bytes = static_cast<std::size_t>(rect.width()) * bytes_per_pixel;
    const std::size_t x_offset = static_cast<std::size_t>(rect.x0) * bytes_per_pixel;
    const std::size_t y_offset = static_cast<std::size_t>(rect.y0) * stride;

    if (rect.x0 == 0 && rect.x1 == info_.width) {
        // Full-width: one memcpy instead of one per row
        memcpy(dst_data + y_offset, src_data + y_offset, row_bytes * static_cast<std::size_t>(rect.height()));
        return;
    }

    for (int row = 0; row < rect.height(); row++) {
        const std::size_t offset = y_offset + static_cast<std::size_t>(row) * stride + x_offset;
        memcpy(dst_data + offset, src_data + offset, row_bytes);
    }
}

void Target::updateStats(bool whole_frame) {
    if (whole_frame) {
        stats_.dirtyRects = 1;
        stats_.dirtyPixels = info_.width * info_.height;
        return;
    }

    stats_.dirtyRects = 0;
    stats_.dirtyPixels = 0;
    for (int i = 0; i < dirty_.count(); i++) {
        const Rect rect = clampRect(dirty_.at(i));
        if (rect.empty()) {
            continue;
        }
        stats_.dirtyRects++;
        stats_.dirtyPixels += rect.width() * rect.height();
    }
}

error_t Target::refresh() {
    if (!valid()) {
        return ERROR_INVALID_STATE;
    }
    return display_refresh(display_);
}

void Target::presentDirty(struct PixelBuffer* buffer) {
    const std::size_t bytes_per_pixel = pixel_buffer_row_stride_bytes(info_.format, 1);
    const std::size_t stride = pixel_buffer_get_stride_bytes(buffer);
    const auto* data = static_cast<const uint8_t*>(pixel_buffer_get_data(buffer));

    for (int i = 0; i < dirty_.count(); i++) {
        const Rect rect = clampRect(dirty_.at(i));
        if (rect.empty()) {
            continue;
        }

        const std::size_t row_bytes = static_cast<std::size_t>(rect.width()) * bytes_per_pixel;

        if (staging_ == nullptr || row_bytes > staging_capacity_) {
            // No staging buffer: present whole rows. Drivers assume the pixels for the rectangle
            // are tightly packed, so a column range of a full-width buffer can't be passed on.
            pixel_buffer_msync(buffer, 0, rect.y0, info_.width, rect.height());
            display_draw_bitmap(display_, 0, rect.y0, info_.width, rect.y1, data + static_cast<std::size_t>(rect.y0) * stride);
            continue;
        }

        const std::size_t x_offset = static_cast<std::size_t>(rect.x0) * bytes_per_pixel;
        const int rows_per_chunk = static_cast<int>(std::max<std::size_t>(staging_capacity_ / row_bytes, 1));

        for (int y = rect.y0; y < rect.y1; y += rows_per_chunk) {
            const int rows = std::min(rows_per_chunk, rect.y1 - y);

            // Pack the rows tightly, because draw_bitmap() has no stride parameter
            for (int row = 0; row < rows; row++) {
                memcpy(
                    staging_ + static_cast<std::size_t>(row) * row_bytes,
                    data + static_cast<std::size_t>(y + row) * stride + x_offset,
                    row_bytes
                );
            }

            display_draw_bitmap(display_, rect.x0, y, rect.x1, y + rows, staging_);
        }
    }
}

bool Target::set_transform(const Transform& transform) {
    if (!valid() || info_.mode == RenderMode::Transform) {
        return false;
    }
    if (transform.width <= 0 || transform.height <= 0) {
        return false;
    }

    const int rotation = ((transform.rotation % 360) + 360) % 360;
    if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) {
        return false;
    }

    // Asking for the native size and orientation means there is nothing to transform
    if (rotation == 0 && transform.width == info_.width && transform.height == info_.height) {
        info_.frameWidth = transform.width;
        info_.frameHeight = transform.height;
        return true;
    }

#if GFX_HAS_PPA
    if (info_.format != DISPLAY_COLOR_FORMAT_RGB565) {
        LOG_W(TAG, "Rotation/scaling needs an RGB565 display");
        return false;
    }

    const MemoryPolicy source_policy { 0, MEMORY_CAPABILITY_EXTERNAL, 64 };
    source_buffer_ = pixel_buffer_create_with_policy(info_.format, transform.width, transform.height, &source_policy);
    if (source_buffer_ == nullptr) {
        return false;
    }

    // Prefer writing the scaler's output straight into a panel frame buffer (no copy, no tearing);
    // otherwise keep a native-size buffer and hand that to draw_bitmap.
    if (flip_buffers_[0] == nullptr) {
        output_buffer_ = pixel_buffer_create_with_policy(info_.format, info_.width, info_.height, &source_policy);
        if (output_buffer_ == nullptr) {
            pixel_buffer_free(source_buffer_);
            source_buffer_ = nullptr;
            return false;
        }
    }

    ppa_client_config_t client_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
        .flags = { .allow_pd = 0 },
    };
    ppa_client_handle_t client = nullptr;
    if (ppa_register_client(&client_config, &client) != ESP_OK) {
        LOG_E(TAG, "Failed to register the PPA client");
        if (source_buffer_ != nullptr) {
            pixel_buffer_free(source_buffer_);
            source_buffer_ = nullptr;
        }
        if (output_buffer_ != nullptr) {
            pixel_buffer_free(output_buffer_);
            output_buffer_ = nullptr;
        }
        return false;
    }
    ppa_client_ = client;

    transform_ = transform;
    transform_.rotation = rotation;
    using_hw_output_ = flip_buffers_[0] != nullptr;
    back_index_ = 0;
    info_.frameWidth = transform.width;
    info_.frameHeight = transform.height;
    info_.mode = RenderMode::Transform;

    LOG_I(TAG, "Rendering %dx%d rotated %d deg onto %dx%d",
        transform_.width, transform_.height, transform_.rotation, info_.width, info_.height);
    return true;
#else
    LOG_W(TAG, "This target has no hardware rotation/scaling");
    return false;
#endif
}

void Target::presentTransformed(struct PixelBuffer* output) {
#if GFX_HAS_PPA
    const int32_t in_w = transform_.width;
    const int32_t in_h = transform_.height;
    const int32_t out_w = info_.width;
    const int32_t out_h = info_.height;

    float scale_x;
    float scale_y;
    ppa_srm_rotation_angle_t angle = PPA_SRM_ROTATION_ANGLE_0;

    // The PPA scales first, then rotates. A quarter turn means the scaled width becomes the
    // output's height (see Doom's 320x200 -> 720x1280 path for the same mapping).
    switch (transform_.rotation) {
        case 90:
            scale_x = static_cast<float>(out_h) / static_cast<float>(in_w);
            scale_y = static_cast<float>(out_w) / static_cast<float>(in_h);
            angle = PPA_SRM_ROTATION_ANGLE_90;
            break;
        case 180:
            scale_x = static_cast<float>(out_w) / static_cast<float>(in_w);
            scale_y = static_cast<float>(out_h) / static_cast<float>(in_h);
            angle = PPA_SRM_ROTATION_ANGLE_180;
            break;
        case 270:
            scale_x = static_cast<float>(out_h) / static_cast<float>(in_w);
            scale_y = static_cast<float>(out_w) / static_cast<float>(in_h);
            angle = PPA_SRM_ROTATION_ANGLE_270;
            break;
        default:
            scale_x = static_cast<float>(out_w) / static_cast<float>(in_w);
            scale_y = static_cast<float>(out_h) / static_cast<float>(in_h);
            break;
    }

    ppa_srm_oper_config_t config = {
        .in = {
            .buffer = pixel_buffer_get_data(source_buffer_),
            .pic_w = static_cast<uint32_t>(in_w),
            .pic_h = static_cast<uint32_t>(in_h),
            .block_w = static_cast<uint32_t>(in_w),
            .block_h = static_cast<uint32_t>(in_h),
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = pixel_buffer_get_data(output),
            .buffer_size = static_cast<uint32_t>(out_w * out_h * 2),
            .pic_w = static_cast<uint32_t>(out_w),
            .pic_h = static_cast<uint32_t>(out_h),
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .rotation_angle = angle,
        .scale_x = scale_x,
        .scale_y = scale_y,
        .mirror_x = false,
        .mirror_y = false,
        .rgb_swap = false,
        .byte_swap = false,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .alpha_fix_val = 0,
        .mode = PPA_TRANS_MODE_BLOCKING,
        .user_data = nullptr,
    };

    if (ppa_do_scale_rotate_mirror(static_cast<ppa_client_handle_t>(ppa_client_), &config) != ESP_OK) {
        LOG_E(TAG, "PPA transfer failed");
    }
#else
    (void)output;
#endif
}

void Target::render(RenderCallback callback, void* context, Prepare prepare) {
    if (!valid() || callback == nullptr) {
        return;
    }

    switch (info_.mode) {
        case RenderMode::Flip: {
            struct PixelBuffer* back = flip_buffers_[back_index_];
            struct PixelBuffer* front = flip_buffers_[1 - back_index_];
            const bool copy_previous = prepare == Prepare::Preserve && flip_has_previous_;
            const int64_t t0 = nowUs();

            // The back buffer holds the frame from two frames ago. Only the regions the previous
            // frame touched differ between the two buffers, so copy just those forward instead of
            // the whole frame - on a 1280x720 panel that's the difference between ~2 MB and a
            // couple of polygon-sized rectangles per frame.
            if (copy_previous) {
                for (int i = 0; i < previous_dirty_.count(); i++) {
                    const Rect rect = clampRect(previous_dirty_.at(i));
                    if (rect.empty()) {
                        continue;
                    }
                    copyRect(back, front, rect);
                }
            }
            const int64_t t1 = nowUs();

            dirty_.clear();
            Surface surface(back, 0, info_.height, &dirty_);
            callback(surface, context);
            const int64_t t2 = nowUs();

            // Write back both the copied regions and the ones drawn this frame. Sync whole rows:
            // pixel_buffer_msync() only takes its one-call contiguous path for full-width ranges,
            // and a sub-rectangle would cost one esp_cache_msync() per row.
            if (copy_previous) {
                for (int i = 0; i < previous_dirty_.count(); i++) {
                    const Rect rect = clampRect(previous_dirty_.at(i));
                    if (!rect.empty()) {
                        pixel_buffer_msync(back, 0, rect.y0, info_.width, rect.height());
                    }
                }
            }
            for (int i = 0; i < dirty_.count(); i++) {
                const Rect rect = clampRect(dirty_.at(i));
                if (!rect.empty()) {
                    pixel_buffer_msync(back, 0, rect.y0, info_.width, rect.height());
                }
            }
            const int64_t t3 = nowUs();

            display_draw_bitmap(display_, 0, 0, info_.width, info_.height, pixel_buffer_get_data(back));
            const int64_t t4 = nowUs();

            back_index_ = 1 - back_index_;
            previous_dirty_ = dirty_;
            flip_has_previous_ = true;
            updateStats(false);            stats_.copyUs = static_cast<int>(t1 - t0);
            stats_.drawUs = static_cast<int>(t2 - t1);
            stats_.syncUs = static_cast<int>(t3 - t2);
            stats_.presentUs = static_cast<int>(t4 - t3);
            stats_.totalUs = static_cast<int>(t4 - t0);
            break;
        }

        case RenderMode::FullFrame: {
            dirty_.clear();

            const int64_t t0 = nowUs();
            Surface surface(frame_buffer_, 0, info_.height, &dirty_);
            callback(surface, context);
            const int64_t t1 = nowUs();

            // The first frame always goes out whole: the panel still shows whatever the previous
            // owner left, and the buffer starts black, so a dirty-region present would leave that
            // content on screen.
            const bool whole_frame = present_full_ || first_frame_;
            if (whole_frame) {
                pixel_buffer_msync(frame_buffer_, 0, 0, info_.width, info_.height);
                display_draw_bitmap(display_, 0, 0, info_.width, info_.height, pixel_buffer_get_data(frame_buffer_));
            } else {
                presentDirty(frame_buffer_);
            }
            first_frame_ = false;
            const int64_t t2 = nowUs();

            updateStats(whole_frame);
            stats_.drawUs = static_cast<int>(t1 - t0);
            stats_.presentUs = static_cast<int>(t2 - t1);
            stats_.totalUs = static_cast<int>(t2 - t0);
            break;
        }

        case RenderMode::Banded: {
            int64_t draw_total = 0;
            int64_t present_total = 0;

            for (int y = 0; y < info_.height; y += info_.bandHeight) {
                const int rows = std::min(info_.bandHeight, info_.height - y);

                pixel_buffer_clear(band_buffer_);
                dirty_.clear();

                const int64_t t0 = nowUs();
                Surface surface(band_buffer_, y, rows, &dirty_);
                callback(surface, context);
                const int64_t t1 = nowUs();

                // Whole band: draw_bitmap() has no stride, so a sub-rectangle of this buffer would
                // read past the end of each row. The band is only a few rows, so this is cheap.
                pixel_buffer_msync(band_buffer_, 0, 0, info_.width, rows);
                display_draw_bitmap(display_, 0, y, info_.width, y + rows, pixel_buffer_get_data(band_buffer_));
                const int64_t t2 = nowUs();

                draw_total += t1 - t0;
                present_total += t2 - t1;
            }

            updateStats(true);
            stats_.drawUs = static_cast<int>(draw_total);
            stats_.presentUs = static_cast<int>(present_total);
            stats_.totalUs = static_cast<int>(draw_total + present_total);
            break;
        }

        case RenderMode::Transform: {
            struct PixelBuffer* output = using_hw_output_ ? flip_buffers_[back_index_] : output_buffer_;

            const int64_t t0 = nowUs();
            Surface surface(source_buffer_, 0, info_.frameHeight, nullptr);
            callback(surface, context);
            const int64_t t1 = nowUs();

            pixel_buffer_msync(source_buffer_, 0, 0, info_.frameWidth, info_.frameHeight);
            presentTransformed(output);
            pixel_buffer_msync(output, 0, 0, info_.width, info_.height);

            display_draw_bitmap(display_, 0, 0, info_.width, info_.height, pixel_buffer_get_data(output));
            const int64_t t2 = nowUs();

            if (using_hw_output_) {
                back_index_ = 1 - back_index_;
            }

            stats_.dirtyRects = 1;
            stats_.dirtyPixels = info_.frameWidth * info_.frameHeight;
            stats_.drawUs = static_cast<int>(t1 - t0);
            stats_.presentUs = static_cast<int>(t2 - t1);
            stats_.totalUs = static_cast<int>(t2 - t0);
            break;
        }

        case RenderMode::None:
            break;
    }
}

} // namespace gfx
