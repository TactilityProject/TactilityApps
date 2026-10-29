// SPDX-License-Identifier: GPL-3.0-or-later
#include "DisplayOutput.h"

#include "../platform/Platform.h"

#include <graphics/pixel_buffer.h>
#include <tactility/memory.h>

#include <algorithm>

namespace maps {

namespace {

/**
 * Strips are in internal DMA memory. In PSRAM the SPI driver copied every strip into a new internal DMA buffer of the
 * same size, which failed while a download used most of the internal RAM, and the map flickered.
 */
constexpr int STRIP_ROWS = 12;

} // namespace

DisplayOutput::~DisplayOutput() {
    end();
}

bool DisplayOutput::begin(Device* displayDevice) {
    display = displayDevice;
    panelWidth = display_get_resolution_x(display);
    panelHeight = display_get_resolution_y(display);
    format = display_get_color_format(display);
    monochrome = format == DISPLAY_COLOR_FORMAT_MONOCHROME || format == DISPLAY_COLOR_FORMAT_GRAYSCALE8;
    fullFrame = format == DISPLAY_COLOR_FORMAT_MONOCHROME || display_has_capability(display, DISPLAY_CAPABILITY_REQUIRES_FULL_FRAME);

    if (display_get_frame_buffer_count(display) == 2) {
        void* first = nullptr;
        void* second = nullptr;
        display_get_frame_buffer(display, 0, &first);
        display_get_frame_buffer(display, 1, &second);
        if (first != nullptr && second != nullptr) {
            hwBuffers[0] = pixel_buffer_wrap(first, format, panelWidth, panelHeight, 0);
            hwBuffers[1] = pixel_buffer_wrap(second, format, panelWidth, panelHeight, 0);
        }
        if (hwBuffers[0] != nullptr && hwBuffers[1] != nullptr) {
            logInfo("Display %dx%d format %d, hardware double buffer", panelWidth, panelHeight, format);
            return true;
        }
        pixel_buffer_free(hwBuffers[0]);
        pixel_buffer_free(hwBuffers[1]);
        hwBuffers[0] = nullptr;
        hwBuffers[1] = nullptr;
    }

    if (fullFrame) {
        frameBuffer = pixel_buffer_create(format, panelWidth, panelHeight);
        logInfo("Display %dx%d format %d, full frames", panelWidth, panelHeight, format);
        return frameBuffer != nullptr;
    }

    const MemoryPolicy stripPolicy = { .required = MEMORY_CAPABILITY_INTERNAL | MEMORY_CAPABILITY_DMA, .desired = 0, .alignment = 64 };
    strips[0] = pixel_buffer_create_with_policy(format, panelWidth, STRIP_ROWS, &stripPolicy);
    strips[1] = pixel_buffer_create_with_policy(format, panelWidth, STRIP_ROWS, &stripPolicy);
    logInfo("Display %dx%d format %d, strips of %d rows", panelWidth, panelHeight, format, STRIP_ROWS);
    return strips[0] != nullptr && strips[1] != nullptr;
}

void DisplayOutput::end() {
    for (auto*& buffer : hwBuffers) {
        pixel_buffer_free(buffer);
        buffer = nullptr;
    }
    for (auto*& buffer : strips) {
        pixel_buffer_free(buffer);
        buffer = nullptr;
    }
    pixel_buffer_free(frameBuffer);
    frameBuffer = nullptr;
    display = nullptr;
}

void DisplayOutput::present(uint16_t* frame, int y, int height) {
    if (display == nullptr) {
        return;
    }
    PixelBuffer* source = pixel_buffer_wrap(frame, DISPLAY_COLOR_FORMAT_RGB565, panelWidth, panelHeight, 0);
    if (source == nullptr) {
        return;
    }
    const auto conversion = monochrome ? PIXEL_BUFFER_CONVERSION_LUMA_THRESHOLD : PIXEL_BUFFER_CONVERSION_AVERAGE;

    if (hwBuffers[0] != nullptr) {
        // The whole frame goes to the back buffer, so it never shows an older frame in the other region
        PixelBuffer* back = hwBuffers[backBuffer];
        pixel_buffer_blit(back, 0, 0, source, 0, 0, panelWidth, panelHeight, conversion);
        pixel_buffer_msync(back, 0, 0, panelWidth, panelHeight);
        display_draw_bitmap(display, 0, 0, panelWidth, panelHeight, pixel_buffer_get_data(back));
        backBuffer = 1 - backBuffer;
    } else if (frameBuffer != nullptr) {
        pixel_buffer_blit(frameBuffer, 0, y, source, 0, y, panelWidth, height, conversion);
        pixel_buffer_msync(frameBuffer, 0, 0, panelWidth, panelHeight);
        display_draw_bitmap(display, 0, 0, panelWidth, panelHeight, pixel_buffer_get_data(frameBuffer));
    } else {
        for (int row = y; row < y + height; row += STRIP_ROWS) {
            const int rows = std::min(STRIP_ROWS, y + height - row);
            PixelBuffer* strip = strips[nextStrip];
            nextStrip = 1 - nextStrip;
            pixel_buffer_blit(strip, 0, 0, source, 0, row, panelWidth, rows, conversion);
            pixel_buffer_msync(strip, 0, 0, panelWidth, rows);
            display_draw_bitmap(display, 0, row, panelWidth, row + rows, pixel_buffer_get_data(strip));
        }
    }
    pixel_buffer_free(source);
}

} // namespace maps
