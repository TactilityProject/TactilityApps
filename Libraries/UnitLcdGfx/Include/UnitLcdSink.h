// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <UnitLcd.h>
#include <gfx/Sink.h>

#include <esp_log.h>

/**
 * Presents a gfx::SinkTarget frame to an M5Stack LCD Unit over I2C.
 *
 * UnitLcd stays unchanged; this adapter lives outside M5UnitModules. The LCD must already be
 * begun and set to the rotation whose width()/height() match the SinkTarget dimensions.
 */
class UnitLcdSink : public gfx::Sink {
public:
    /**
     * @param lcd the unit, already begun and rotated
     * @param originX left edge of the sub-frame within the LCD
     * @param originY top edge of the sub-frame within the LCD
     */
    explicit UnitLcdSink(UnitLcd& lcd, int originX = 0, int originY = 0)
        : lcd_(lcd), originX_(originX), originY_(originY) {}

    bool begin(const gfx::SinkInfo& info) override {
        if (!lcd_.isPresent()) {
            ESP_LOGW("UnitLcdSink", "begin: LCD not present");
            return false;
        }
        if (info.format != DISPLAY_COLOR_FORMAT_RGB565) {
            ESP_LOGW("UnitLcdSink", "begin: format %d is not RGB565", static_cast<int>(info.format));
            return false;
        }
        if (originX_ < 0 || originY_ < 0 || info.width <= 0 || info.height <= 0) {
            ESP_LOGW("UnitLcdSink", "begin: invalid sub-frame %dx%d at (%d, %d)", info.width, info.height, originX_, originY_);
            return false;
        }
        if (originX_ + info.width > static_cast<int>(lcd_.width()) || originY_ + info.height > static_cast<int>(lcd_.height())) {
            ESP_LOGW(
                "UnitLcdSink",
                "begin: sub-frame %dx%d at (%d, %d) exceeds LCD %ux%u",
                info.width,
                info.height,
                originX_,
                originY_,
                static_cast<unsigned>(lcd_.width()),
                static_cast<unsigned>(lcd_.height())
            );
            return false;
        }
        info_ = info;
        begun_ = true;
        return true;
    }

    bool present(const struct PixelBuffer& buffer, const gfx::Rect& rect) override {
        if (!begun_) {
            return false;
        }

        const gfx::Rect clipped = rect.intersect(gfx::Rect { 0, 0, info_.width, info_.height });
        if (clipped.empty()) {
            return true;
        }

        if (!lcd_.setWindow(
                static_cast<uint8_t>(originX_ + clipped.x0),
                static_cast<uint8_t>(originY_ + clipped.y0),
                static_cast<uint8_t>(originX_ + clipped.x1 - 1),
                static_cast<uint8_t>(originY_ + clipped.y1 - 1))) {
            return false;
        }

        auto* mutable_buffer = const_cast<struct PixelBuffer*>(&buffer);
        const int bufferWidth = pixel_buffer_get_width(&buffer);
        const std::size_t stride = pixel_buffer_get_stride_bytes(&buffer);
        const auto* data = static_cast<const uint16_t*>(pixel_buffer_get_data(mutable_buffer));

        // Full-width rows are contiguous in a PixelBuffer. Send them as one stream instead of one
        // I2C transaction per row; the first full-frame present is much faster this way.
        if (clipped.x0 == 0 && clipped.x1 == bufferWidth && stride == static_cast<std::size_t>(bufferWidth) * sizeof(uint16_t)) {
            lcd_.writePixels(
                data + static_cast<std::size_t>(clipped.y0) * bufferWidth,
                static_cast<uint32_t>(bufferWidth) * static_cast<uint32_t>(clipped.height())
            );
            return true;
        }

        for (int y = clipped.y0; y < clipped.y1; y++) {
            const auto* row = static_cast<const uint16_t*>(pixel_buffer_get_row(mutable_buffer, y));
            lcd_.writePixels(row + clipped.x0, static_cast<uint32_t>(clipped.width()));
        }
        return true;
    }

    void end() override {
        begun_ = false;
    }

private:
    UnitLcd& lcd_;
    int originX_ = 0;
    int originY_ = 0;
    gfx::SinkInfo info_ {};
    bool begun_ = false;
};
