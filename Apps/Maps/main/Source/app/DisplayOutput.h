// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <tactility/device.h>
#include <tactility/drivers/display.h>

#include <cstdint>

struct PixelBuffer;

namespace maps {

/**
 * Sends an RGB565 frame to the panel in its own colour format. Uses the hardware double buffer when the driver
 * has one, a full frame buffer when the panel needs full frames, and otherwise small strips.
 */
class DisplayOutput {
public:
    ~DisplayOutput();

    bool begin(Device* displayDevice);
    void end();

    /** Sends rows y to y + height of a frame of width() x height() RGB565 pixels. */
    void present(uint16_t* frame, int y, int height);

    int width() const { return panelWidth; }
    int height() const { return panelHeight; }
    bool isMonochrome() const { return monochrome; }

private:
    Device* display = nullptr;
    int panelWidth = 0;
    int panelHeight = 0;
    DisplayColorFormat format = DISPLAY_COLOR_FORMAT_RGB565;
    bool monochrome = false;
    bool fullFrame = false;
    PixelBuffer* hwBuffers[2] = { nullptr, nullptr };
    int backBuffer = 1;
    PixelBuffer* frameBuffer = nullptr;
    PixelBuffer* strips[2] = { nullptr, nullptr };
    int nextStrip = 0;
};

} // namespace maps
