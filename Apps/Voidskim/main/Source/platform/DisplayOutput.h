#pragma once

#include <tactility/device.h>

#include <cstdint>

/**
 * How the user sees the display, captured from LVGL before it's stopped (see main.cpp).
 * Rotations are in quarter turns counter-clockwise (LVGL's and the PPA's convention), from the
 * user's view to the coordinate space in question.
 */
struct ScreenInfo {
    int drawRotation = 0; // user view -> display_draw_bitmap() coordinates
    uint32_t drawWidth = 0;
    uint32_t drawHeight = 0;
    int touchRotation = 0; // user view -> raw touch coordinates
    uint32_t touchWidth = 0;
    uint32_t touchHeight = 0;
};

/**
 * Shows the game's RGB565 frame on the display. The game renders at a reduced resolution with the
 * display's aspect ratio (software 3D is expensive per pixel); this scales it up, rotates it to the
 * panel's orientation and converts it to the panel's pixel format. Uses the PPA where the chip has
 * one (ESP32-P4/S31), software otherwise.
 */
class DisplayOutput {
public:
    bool init(Device* display, const ScreenInfo& screen);
    void shutdown();

    /** The frame to render into: width() x height() RGB565 pixels. */
    uint16_t* frame() { return frame_; }
    int width() const { return frameWidth; }
    int height() const { return frameHeight; }

    /**
     * Shows the frame.
     * @param uiMask on monochrome displays: frame pixels that are UI (1) rather than 3D world (0),
     * or nullptr to dither everything
     */
    void present(const uint8_t* uiMask = nullptr);

    /** @return whether the display is black and white (or greyscale), so frames get dithered */
    bool isMonochrome() const;

    /** Maps a raw touch coordinate to frame coordinates. @return false when outside the frame */
    bool mapTouch(int touchX, int touchY, int& outX, int& outY) const;

private:
    Device* display = nullptr;
    ScreenInfo screen;
    uint16_t* frame_ = nullptr;
    int frameWidth = 0;
    int frameHeight = 0;
    int userWidth = 0;
    int userHeight = 0;
    float scale = 1.0f;
    // Where the scaled frame lands: user coordinates, then draw coordinates
    int ux = 0, uy = 0, uw = 0, uh = 0;
    int dx = 0, dy = 0, dw = 0, dh = 0;
    uint16_t* xMap = nullptr;
    uint16_t* yMap = nullptr;

    bool initOutput();
    void presentSoftware(const uint8_t* uiMask);
};
