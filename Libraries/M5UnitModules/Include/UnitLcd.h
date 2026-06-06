/*
 * UnitLcd.h - M5Stack LCD Unit (ESP32-PICO inside, I2C addr 0x3E)
 *
 * 135×240 IPS display driven by an internal ESP32 that bridges I2C commands
 * to an ST7789V2. Unlike the STM32-based units there is no register map -
 * instead you send drawing commands into a command buffer. The internal ESP32
 * processes them independently, so writes are fire-and-forget for most ops.
 *
 * Key differences from STM32 units:
 *   - No STOP+delay+read pattern - standard repeated-START reads work fine
 *   - No 2ms inter-transaction delay needed
 *   - Pixel data goes in a single uninterrupted I2C transaction (WRITE_RAW)
 *   - Check READ_BUFCOUNT (0x09) before large writes to avoid overflow
 *   - Max I2C clock: 400 kHz
 *
 * Physical display: 135 wide × 240 tall (portrait, rotation 0/2)
 *                   240 wide × 135 tall (landscape, rotation 1/3)
 *
 * Coordinate system: origin top-left, X right, Y down, in the current
 * rotation's logical space. Use width()/height() for safe bounds.
 *
 * Command quick-reference:
 *   0x22 [brightness]                  Set backlight (0=off, 255=max)
 *   0x36 [rotation]                    Set rotation (0-3 = 0/90/180/270°)
 *   0x2A [x0] [x1]                    Set X window (column range, inclusive)
 *   0x2B [y0] [y1]                    Set Y window (row range, inclusive)
 *   0x68 [x0][y0][x1][y1]             Fill rect with stored colour
 *   0x6A [x0][y0][x1][y1][hi][lo]     Fill rect with RGB565 colour inline
 *   0x62 [x][y][hi][lo]                Draw pixel RGB565
 *   0x42 [pixels...]                   Write raw RGB565 stream (STOP ends it)
 *   0x09                               Read buffer remaining count (1 byte)
 */
#pragma once

#include <UnitCommon.h>
#include <cstdint>

class UnitLcd {
public:
    static constexpr uint8_t  DEFAULT_ADDR    = 0x3E;
    static constexpr uint16_t PHYS_WIDTH      = 135;
    static constexpr uint16_t PHYS_HEIGHT     = 240;

    // Legacy constants - equal to physical portrait dimensions.
    static constexpr uint16_t WIDTH  = PHYS_WIDTH;
    static constexpr uint16_t HEIGHT = PHYS_HEIGHT;

    UnitLcd() = default;
    UnitLcd(const UnitLcd&) = delete;
    UnitLcd& operator=(const UnitLcd&) = delete;

    // Pass a I2C controller device
    // Probe and initialise. Sets brightness to 128, rotation to 0.
    [[nodiscard]] bool begin(Device* dev, uint8_t addr = DEFAULT_ADDR);

    bool isPresent() const { return dev_ != nullptr; }

    // Logical dimensions in the current rotation's coordinate space.
    uint16_t width()  const { return (rotation_ & 1) ? PHYS_HEIGHT : PHYS_WIDTH; }
    uint16_t height() const { return (rotation_ & 1) ? PHYS_WIDTH  : PHYS_HEIGHT; }
    uint8_t  rotation() const { return rotation_; }

    // -----------------------------------------------------------------------
    // Control
    // -----------------------------------------------------------------------

    void setBrightness(uint8_t brightness);

    // Rotation: 0=portrait(135×240), 1=landscape(240×135),
    //           2=portrait flipped,  3=landscape flipped.
    void setRotation(uint8_t rotation);

    // -----------------------------------------------------------------------
    // Filled primitives (fast hardware commands)
    // -----------------------------------------------------------------------

    void fillScreen(uint16_t rgb565);
    void fillRect(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, uint16_t rgb565);
    void drawPixel(uint8_t x, uint8_t y, uint16_t rgb565);

    // -----------------------------------------------------------------------
    // Raw pixel streaming
    // -----------------------------------------------------------------------

    bool setWindow(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1);
    void writePixels(const uint16_t* pixels, uint32_t len);

    // -----------------------------------------------------------------------
    // Software-rendered shapes (Bresenham / midpoint algorithms)
    // -----------------------------------------------------------------------

    // Horizontal / vertical lines (faster than drawLine for axis-aligned).
    void drawHLine(uint8_t x, uint8_t y, uint8_t len, uint16_t rgb565);
    void drawVLine(uint8_t x, uint8_t y, uint8_t len, uint16_t rgb565);

    // Arbitrary line (Bresenham).
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t rgb565);

    // Rectangle outline.
    void drawRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t rgb565);

    // Filled / outline circle (midpoint algorithm).
    void fillCircle(int16_t cx, int16_t cy, int16_t r, uint16_t rgb565);
    void drawCircle(int16_t cx, int16_t cy, int16_t r, uint16_t rgb565);

    // Rounded rectangle (filled / outline).
    void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t rgb565);
    void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t rgb565);

    // Triangle (filled / outline).
    void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                      int16_t x2, int16_t y2, uint16_t rgb565);
    void drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                      int16_t x2, int16_t y2, uint16_t rgb565);

    // Arc - angles in degrees (0=right, 90=down, 180=left, 270=up).
    // r0 = outer radius, r1 = inner radius (0 for solid filled wedge).
    // fillArc draws a filled arc/wedge; drawArc draws the outline only.
    void fillArc(int16_t cx, int16_t cy, int16_t r0, int16_t r1,
                 float startDeg, float endDeg, uint16_t rgb565);
    void drawArc(int16_t cx, int16_t cy, int16_t r0, int16_t r1,
                 float startDeg, float endDeg, uint16_t rgb565);

    // -----------------------------------------------------------------------
    // Text rendering - 5×7 bitmap font (ASCII 32-126)
    // -----------------------------------------------------------------------

    // Draw a single character at (x, y). scale 1=5×7 px, 2=10×14 px, …
    void drawChar(uint8_t x, uint8_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale = 1);

    // Draw a null-terminated string starting at (x, y), advancing 6*scale px per char.
    void drawText(uint8_t x, uint8_t y, const char* str, uint16_t fg, uint16_t bg, uint8_t scale = 1);

    // -----------------------------------------------------------------------
    // Misc
    // -----------------------------------------------------------------------

    uint8_t bufferRemaining();

    static uint16_t rgb888to565(uint32_t rgb888) {
        uint8_t r = (rgb888 >> 16) & 0xFF;
        uint8_t g = (rgb888 >>  8) & 0xFF;
        uint8_t b =  rgb888        & 0xFF;
        return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }

    // color565 alias used by M5GFX-style code
    static uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
        return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }

private:
    Device*  dev_      = nullptr;
    uint8_t  addr_     = DEFAULT_ADDR;
    uint8_t  rotation_ = 0;

    bool sendCmd(const uint8_t* data, uint16_t len);

    // Helper: draw pixel only if in bounds (used by circle/line algorithms).
    void plotPixel(int16_t x, int16_t y, uint16_t rgb565);

    // Helper: fill horizontal span, clamped to screen.
    void hspan(int16_t x, int16_t y, int16_t len, uint16_t rgb565);

    // Octant helper for circle algorithms.
    void circleOctants(int16_t cx, int16_t cy, int16_t x, int16_t y,
                       uint16_t rgb565, bool fill);

    // Arc pixel helper - plots or fills one pixel/column depending on fill flag.
    void arcImpl(int16_t cx, int16_t cy, int16_t r0, int16_t r1,
                 float startDeg, float endDeg, uint16_t rgb565, bool fill);

    // ESP32 I2C driver buffer is 256 bytes. With 1 command byte, 255 bytes remain.
    // 255 / 2 bytes per pixel = 127, rounded down to 126 for alignment.
    static constexpr uint16_t CHUNK_PIXELS = 126;
};
