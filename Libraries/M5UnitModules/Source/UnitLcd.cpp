#include <UnitLcd.h>
#include <esp_log.h>
#include <cstring>
#include <cmath>
#include <algorithm>

static constexpr auto* TAG = "UnitLcd";

static constexpr uint8_t CMD_SET_BRIGHTNESS = 0x22;
static constexpr uint8_t CMD_SET_ROTATION   = 0x36;
static constexpr uint8_t CMD_FILL_RECT      = 0x6A;
static constexpr uint8_t CMD_DRAW_PIXEL     = 0x62;
static constexpr uint8_t CMD_SET_COL_RANGE  = 0x2A;
static constexpr uint8_t CMD_SET_ROW_RANGE  = 0x2B;
static constexpr uint8_t CMD_WRITE_RAW      = 0x42;
static constexpr uint8_t CMD_READ_BUFCOUNT  = 0x09;

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

bool UnitLcd::sendCmd(const uint8_t* data, uint16_t len) {
    return i2c_controller_write(dev_, addr_, data, len,
                                pdMS_TO_TICKS(UNIT_I2C_TIMEOUT_MS)) == ERROR_NONE;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bool UnitLcd::begin(Device* dev, uint8_t addr) {
    if (!dev || !device_is_ready(dev)) return false;
    if (!unitProbe(dev, addr)) {
        ESP_LOGW(TAG, "LCD unit not found at 0x%02X", addr);
        return false;
    }
    dev_      = dev;
    addr_     = addr;
    rotation_ = 0;
    uint8_t brCmd[2] = { CMD_SET_BRIGHTNESS, 128 };
    if (!sendCmd(brCmd, 2)) {
        ESP_LOGE(TAG, "LCD setBrightness failed at 0x%02X", addr_);
        dev_ = nullptr;
        return false;
    }
    uint8_t rotCmd[2] = { CMD_SET_ROTATION, 0x00 };
    if (!sendCmd(rotCmd, 2)) {
        ESP_LOGE(TAG, "LCD setRotation failed at 0x%02X", addr_);
        dev_ = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "LCD unit ready at 0x%02X", addr_);
    return true;
}

// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------

void UnitLcd::setBrightness(uint8_t brightness) {
    if (!dev_) return;
    uint8_t cmd[2] = { CMD_SET_BRIGHTNESS, brightness };
    if (!sendCmd(cmd, 2))
        ESP_LOGW(TAG, "setBrightness cmd failed");
}

void UnitLcd::setRotation(uint8_t rot) {
    if (!dev_) return;
    rotation_ = rot & 0x03;
    uint8_t cmd[2] = { CMD_SET_ROTATION, (uint8_t)(rotation_ & 0x07) };
    if (!sendCmd(cmd, 2))
        ESP_LOGW(TAG, "setRotation cmd failed");
}

// ---------------------------------------------------------------------------
// Filled primitives
// ---------------------------------------------------------------------------

void UnitLcd::fillRect(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, uint16_t rgb565) {
    if (!dev_) return;
    uint8_t cmd[7] = {
        CMD_FILL_RECT,
        x0, y0, x1, y1,
        (uint8_t)(rgb565 >> 8),
        (uint8_t)(rgb565 & 0xFF)
    };
    sendCmd(cmd, 7);
}

void UnitLcd::fillScreen(uint16_t rgb565) {
    if (!dev_) return;
    fillRect(0, 0, (uint8_t)(width() - 1), (uint8_t)(height() - 1), rgb565);
}

void UnitLcd::drawPixel(uint8_t x, uint8_t y, uint16_t rgb565) {
    if (!dev_) return;
    uint8_t cmd[5] = {
        CMD_DRAW_PIXEL,
        x, y,
        (uint8_t)(rgb565 >> 8),
        (uint8_t)(rgb565 & 0xFF)
    };
    sendCmd(cmd, 5);
}

// ---------------------------------------------------------------------------
// Raw pixel streaming
// ---------------------------------------------------------------------------

bool UnitLcd::setWindow(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1) {
    if (!dev_) return false;
    uint8_t caset[3] = { CMD_SET_COL_RANGE, x0, x1 };
    uint8_t raset[3] = { CMD_SET_ROW_RANGE, y0, y1 };
    return sendCmd(caset, 3) && sendCmd(raset, 3);
}

void UnitLcd::writePixels(const uint16_t* pixels, uint32_t len) {
    if (!dev_ || !pixels || len == 0) return;
    uint32_t offset = 0;
    while (offset < len) {
        uint32_t chunk = std::min((uint32_t)CHUNK_PIXELS, len - offset);
        uint8_t pkt[1 + CHUNK_PIXELS * 2];
        pkt[0] = CMD_WRITE_RAW;
        for (uint32_t i = 0; i < chunk; i++) {
            uint16_t px = pixels[offset + i];
            pkt[1 + i*2 + 0] = (uint8_t)(px >> 8);
            pkt[1 + i*2 + 1] = (uint8_t)(px & 0xFF);
        }
        sendCmd(pkt, (uint16_t)(1 + chunk * 2));
        offset += chunk;
    }
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

uint8_t UnitLcd::bufferRemaining() {
    if (!dev_) return UINT8_MAX;
    uint8_t cmd = CMD_READ_BUFCOUNT;
    if (i2c_controller_write(dev_, addr_, &cmd, 1,
                             pdMS_TO_TICKS(UNIT_I2C_TIMEOUT_MS)) != ERROR_NONE)
        return UINT8_MAX;
    uint8_t val = 0;
    if (i2c_controller_read(dev_, addr_, &val, 1,
                            pdMS_TO_TICKS(UNIT_I2C_TIMEOUT_MS)) != ERROR_NONE)
        return UINT8_MAX;
    return val;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void UnitLcd::plotPixel(int16_t x, int16_t y, uint16_t rgb565) {
    if (x < 0 || y < 0 || x >= (int16_t)width() || y >= (int16_t)height()) return;
    drawPixel((uint8_t)x, (uint8_t)y, rgb565);
}

void UnitLcd::hspan(int16_t x, int16_t y, int16_t len, uint16_t rgb565) {
    if (y < 0 || y >= (int16_t)height() || len <= 0) return;
    int16_t x1 = x + len - 1;
    if (x  < 0) x  = 0;
    if (x1 >= (int16_t)width()) x1 = (int16_t)width() - 1;
    if (x > x1) return;
    fillRect((uint8_t)x, (uint8_t)y, (uint8_t)x1, (uint8_t)y, rgb565);
}

// ---------------------------------------------------------------------------
// Lines
// ---------------------------------------------------------------------------

void UnitLcd::drawHLine(uint8_t x, uint8_t y, uint8_t len, uint16_t rgb565) {
    if (!dev_ || len == 0) return;
    if (x >= width()) return;
    uint16_t x1 = (uint16_t)x + len - 1;
    if (x1 >= width()) x1 = width() - 1;
    fillRect(x, y, (uint8_t)x1, y, rgb565);
}

void UnitLcd::drawVLine(uint8_t x, uint8_t y, uint8_t len, uint16_t rgb565) {
    if (!dev_ || len == 0) return;
    if (y >= height()) return;
    uint16_t y1 = (uint16_t)y + len - 1;
    if (y1 >= height()) y1 = height() - 1;
    fillRect(x, y, x, (uint8_t)y1, rgb565);
}

void UnitLcd::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t rgb565) {
    if (!dev_) return;
    // Fast paths
    if (y0 == y1) { hspan(std::min(x0, x1), y0, (int16_t)std::abs(x1 - x0) + 1, rgb565); return; }
    if (x0 == x1) {
        int16_t ylo = std::min(y0, y1), yhi = std::max(y0, y1);
        for (int16_t y = ylo; y <= yhi; y++) plotPixel(x0, y, rgb565);
        return;
    }
    // Bresenham
    int16_t dx =  std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int16_t dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int16_t err = dx + dy;
    while (true) {
        plotPixel(x0, y0, rgb565);
        if (x0 == x1 && y0 == y1) break;
        int16_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// ---------------------------------------------------------------------------
// Rectangle outline
// ---------------------------------------------------------------------------

void UnitLcd::drawRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t rgb565) {
    if (!dev_ || w == 0 || h == 0) return;
    drawHLine(x,         y,         w, rgb565);
    drawHLine(x,         y + h - 1, w, rgb565);
    drawVLine(x,         y,         h, rgb565);
    drawVLine(x + w - 1, y,         h, rgb565);
}

// ---------------------------------------------------------------------------
// Circle helpers
// ---------------------------------------------------------------------------

void UnitLcd::circleOctants(int16_t cx, int16_t cy, int16_t xi, int16_t yi,
                             uint16_t rgb565, bool fill) {
    if (fill) {
        hspan(cx - xi, cy + yi, 2 * xi + 1, rgb565);
        hspan(cx - xi, cy - yi, 2 * xi + 1, rgb565);
        hspan(cx - yi, cy + xi, 2 * yi + 1, rgb565);
        hspan(cx - yi, cy - xi, 2 * yi + 1, rgb565);
    } else {
        plotPixel(cx + xi, cy + yi, rgb565); plotPixel(cx - xi, cy + yi, rgb565);
        plotPixel(cx + xi, cy - yi, rgb565); plotPixel(cx - xi, cy - yi, rgb565);
        plotPixel(cx + yi, cy + xi, rgb565); plotPixel(cx - yi, cy + xi, rgb565);
        plotPixel(cx + yi, cy - xi, rgb565); plotPixel(cx - yi, cy - xi, rgb565);
    }
}

static void midpointCircle(int16_t r, int16_t& xi, int16_t& yi, int16_t& d) {
    xi = 0; yi = r; d = 1 - r;
}

void UnitLcd::fillCircle(int16_t cx, int16_t cy, int16_t r, uint16_t rgb565) {
    if (!dev_ || r < 0) return;
    int16_t xi, yi, d;
    midpointCircle(r, xi, yi, d);
    while (xi <= yi) {
        circleOctants(cx, cy, xi, yi, rgb565, true);
        xi++;
        if (d < 0) { d += 2 * xi + 1; }
        else { yi--; d += 2 * (xi - yi) + 1; }
    }
}

void UnitLcd::drawCircle(int16_t cx, int16_t cy, int16_t r, uint16_t rgb565) {
    if (!dev_ || r < 0) return;
    int16_t xi, yi, d;
    midpointCircle(r, xi, yi, d);
    while (xi <= yi) {
        circleOctants(cx, cy, xi, yi, rgb565, false);
        xi++;
        if (d < 0) { d += 2 * xi + 1; }
        else { yi--; d += 2 * (xi - yi) + 1; }
    }
}

// ---------------------------------------------------------------------------
// Rounded rectangles
// ---------------------------------------------------------------------------

void UnitLcd::fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h,
                             int16_t r, uint16_t rgb565) {
    if (!dev_) return;
    if (r <= 0 || 2*r > w || 2*r > h) {
        fillRect((uint8_t)x, (uint8_t)y, (uint8_t)(x+w-1), (uint8_t)(y+h-1), rgb565);
        return;
    }
    // Two vertical rectangles covering the centre + top/bottom straight sections
    fillRect((uint8_t)(x + r), (uint8_t)y,     (uint8_t)(x + w - r - 1), (uint8_t)(y + h - 1), rgb565);
    fillRect((uint8_t)x,       (uint8_t)(y + r),(uint8_t)(x + r - 1),    (uint8_t)(y + h - r - 1), rgb565);
    fillRect((uint8_t)(x+w-r), (uint8_t)(y + r),(uint8_t)(x + w - 1),   (uint8_t)(y + h - r - 1), rgb565);
    // Corner arc spans
    int16_t xi = 0, yi = r, d = 1 - r;
    while (xi <= yi) {
        // Top-left / top-right arcs
        hspan(x + r - xi, y + r - yi, w - 2*(r - xi), rgb565);
        // Bottom-left / bottom-right arcs
        hspan(x + r - xi, y + h - 1 - r + yi, w - 2*(r - xi), rgb565);
        if (xi != yi) {
            hspan(x + r - yi, y + r - xi, w - 2*(r - yi), rgb565);
            hspan(x + r - yi, y + h - 1 - r + xi, w - 2*(r - yi), rgb565);
        }
        xi++;
        if (d < 0) d += 2*xi + 1; else { yi--; d += 2*(xi-yi)+1; }
    }
}

void UnitLcd::drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h,
                             int16_t r, uint16_t rgb565) {
    if (!dev_) return;
    if (r <= 0 || 2*r > w || 2*r > h) { drawRect((uint8_t)x, (uint8_t)y, (uint8_t)w, (uint8_t)h, rgb565); return; }
    // Straight edges
    drawHLine((uint8_t)(x+r), (uint8_t)y,       (uint8_t)(w - 2*r), rgb565);
    drawHLine((uint8_t)(x+r), (uint8_t)(y+h-1), (uint8_t)(w - 2*r), rgb565);
    drawVLine((uint8_t)x,     (uint8_t)(y+r),   (uint8_t)(h - 2*r), rgb565);
    drawVLine((uint8_t)(x+w-1),(uint8_t)(y+r),  (uint8_t)(h - 2*r), rgb565);
    // Corner arcs
    int16_t xi = 0, yi = r, d = 1 - r;
    while (xi <= yi) {
        plotPixel(x+r-xi,   y+r-yi,   rgb565); plotPixel(x+w-r+xi-1, y+r-yi,   rgb565);
        plotPixel(x+r-xi,   y+h-r+yi-1,rgb565); plotPixel(x+w-r+xi-1, y+h-r+yi-1,rgb565);
        plotPixel(x+r-yi,   y+r-xi,   rgb565); plotPixel(x+w-r+yi-1, y+r-xi,   rgb565);
        plotPixel(x+r-yi,   y+h-r+xi-1,rgb565); plotPixel(x+w-r+yi-1, y+h-r+xi-1,rgb565);
        xi++;
        if (d < 0) d += 2*xi+1; else { yi--; d += 2*(xi-yi)+1; }
    }
}

// ---------------------------------------------------------------------------
// Triangles
// ---------------------------------------------------------------------------

void UnitLcd::drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                            int16_t x2, int16_t y2, uint16_t rgb565) {
    drawLine(x0, y0, x1, y1, rgb565);
    drawLine(x1, y1, x2, y2, rgb565);
    drawLine(x2, y2, x0, y0, rgb565);
}

void UnitLcd::fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                            int16_t x2, int16_t y2, uint16_t rgb565) {
    if (!dev_) return;
    // Sort vertices by Y (bubble sort, 3 elements)
    if (y0 > y1) { std::swap(x0,x1); std::swap(y0,y1); }
    if (y1 > y2) { std::swap(x1,x2); std::swap(y1,y2); }
    if (y0 > y1) { std::swap(x0,x1); std::swap(y0,y1); }

    if (y0 == y2) { // degenerate horizontal line
        int16_t xlo = std::min({x0,x1,x2}), xhi = std::max({x0,x1,x2});
        hspan(xlo, y0, xhi - xlo + 1, rgb565);
        return;
    }

    // Scan-line fill using integer fixed-point slopes (×16 precision)
    int32_t dx02 = ((int32_t)(x2 - x0) << 4) / (y2 - y0);
    int32_t xa   = ((int32_t)x0 << 4);

    if (y1 == y0) {
        // Flat top
        int32_t dx12 = ((int32_t)(x2 - x1) << 4) / (y2 - y1);
        int32_t xb   = ((int32_t)x1 << 4);
        for (int16_t y = y0; y <= y2; y++) {
            int16_t xlo = (int16_t)(xa >> 4), xhi = (int16_t)(xb >> 4);
            if (xlo > xhi) std::swap(xlo, xhi);
            hspan(xlo, y, xhi - xlo + 1, rgb565);
            xa += dx02; xb += dx12;
        }
    } else if (y1 == y2) {
        // Flat bottom
        int32_t dx01 = ((int32_t)(x1 - x0) << 4) / (y1 - y0);
        int32_t xb   = ((int32_t)x0 << 4);
        for (int16_t y = y0; y <= y1; y++) {
            int16_t xlo = (int16_t)(xa >> 4), xhi = (int16_t)(xb >> 4);
            if (xlo > xhi) std::swap(xlo, xhi);
            hspan(xlo, y, xhi - xlo + 1, rgb565);
            xa += dx02; xb += dx01;
        }
    } else {
        // General: upper half then lower half
        int32_t dx01 = ((int32_t)(x1 - x0) << 4) / (y1 - y0);
        int32_t xb   = ((int32_t)x0 << 4);
        for (int16_t y = y0; y < y1; y++) {
            int16_t xlo = (int16_t)(xa >> 4), xhi = (int16_t)(xb >> 4);
            if (xlo > xhi) std::swap(xlo, xhi);
            hspan(xlo, y, xhi - xlo + 1, rgb565);
            xa += dx02; xb += dx01;
        }
        int32_t dx12 = ((int32_t)(x2 - x1) << 4) / (y2 - y1);
        xb = ((int32_t)x1 << 4);
        for (int16_t y = y1; y <= y2; y++) {
            int16_t xlo = (int16_t)(xa >> 4), xhi = (int16_t)(xb >> 4);
            if (xlo > xhi) std::swap(xlo, xhi);
            hspan(xlo, y, xhi - xlo + 1, rgb565);
            xa += dx02; xb += dx12;
        }
    }
}

// ---------------------------------------------------------------------------
// Arc (filled annular wedge / outline)
// ---------------------------------------------------------------------------
// Implemented by scanning every pixel in the bounding box of the outer circle
// and testing (a) whether it falls within the annular ring r1..r0, and
// (b) whether the pixel's angle falls within startDeg..endDeg.
// For a 135×240 display this is at most 135*135 ≈ 18k pixels per call -
// slow compared to hardware fill, but correct and free of floating-point
// arc-length accumulation errors.

static constexpr float ARC_DEG2RAD = 3.14159265f / 180.0f;

void UnitLcd::arcImpl(int16_t cx, int16_t cy, int16_t r0, int16_t r1,
                      float startDeg, float endDeg, uint16_t rgb565, bool fill) {
    if (!dev_ || r0 <= 0) return;
    if (r1 < 0) r1 = 0;
    if (r1 > r0) std::swap(r0, r1);

    // Normalise angles to [0, 360)
    startDeg = fmodf(startDeg, 360.0f);
    if (startDeg < 0) startDeg += 360.0f;
    endDeg = fmodf(endDeg, 360.0f);
    if (endDeg < 0) endDeg += 360.0f;
    bool wraps = (endDeg <= startDeg);  // arc crosses 0°

    int32_t r0sq = (int32_t)r0 * r0;
    int32_t r1sq = (int32_t)r1 * r1;

    int16_t W = (int16_t)width(), H = (int16_t)height();

    for (int16_t y = -r0; y <= r0; y++) {
        int16_t py = cy + y;
        if (py < 0 || py >= H) continue;
        for (int16_t x = -r0; x <= r0; x++) {
            int16_t px = cx + x;
            if (px < 0 || px >= W) continue;
            int32_t d2 = (int32_t)x * x + (int32_t)y * y;
            if (d2 > r0sq) continue;
            if (fill) {
                if (d2 < r1sq) continue;
            } else {
                // Outline: only pixels on the outer ring edge or radial endpoints
                // outer ring: r0-1 < dist <= r0
                bool onOuter = (d2 > (int32_t)(r0-1)*(r0-1));
                bool onInner = (r1 > 0) && (d2 >= r1sq) && (d2 < (int32_t)(r1+1)*(r1+1));
                if (!onOuter && !onInner) continue;
            }
            // Angle check (atan2 returns -π..π, convert to 0..360)
            float ang = atan2f((float)y, (float)x) / ARC_DEG2RAD;
            if (ang < 0) ang += 360.0f;
            bool inSweep;
            if (!wraps) inSweep = (ang >= startDeg && ang <= endDeg);
            else        inSweep = (ang >= startDeg || ang <= endDeg);
            if (!inSweep) continue;
            drawPixel((uint8_t)px, (uint8_t)py, rgb565);
        }
    }
}

void UnitLcd::fillArc(int16_t cx, int16_t cy, int16_t r0, int16_t r1,
                      float startDeg, float endDeg, uint16_t rgb565) {
    arcImpl(cx, cy, r0, r1, startDeg, endDeg, rgb565, true);
}

void UnitLcd::drawArc(int16_t cx, int16_t cy, int16_t r0, int16_t r1,
                      float startDeg, float endDeg, uint16_t rgb565) {
    arcImpl(cx, cy, r0, r1, startDeg, endDeg, rgb565, false);
}

// ---------------------------------------------------------------------------
// Text rendering - minimal 5×7 bitmap font (ASCII 32-126)
// Each entry is 5 bytes: one byte per column (bit 0 = top row).
// ---------------------------------------------------------------------------

static const uint8_t FONT5X7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x00,0x00,0x5F,0x00,0x00}, // '!'
    {0x00,0x07,0x00,0x07,0x00}, // '"'
    {0x14,0x7F,0x14,0x7F,0x14}, // '#'
    {0x24,0x2A,0x7F,0x2A,0x12}, // '$'
    {0x23,0x13,0x08,0x64,0x62}, // '%'
    {0x36,0x49,0x55,0x22,0x50}, // '&'
    {0x00,0x05,0x03,0x00,0x00}, // '\''
    {0x00,0x1C,0x22,0x41,0x00}, // '('
    {0x00,0x41,0x22,0x1C,0x00}, // ')'
    {0x08,0x2A,0x1C,0x2A,0x08}, // '*'
    {0x08,0x08,0x3E,0x08,0x08}, // '+'
    {0x00,0x50,0x30,0x00,0x00}, // ','
    {0x08,0x08,0x08,0x08,0x08}, // '-'
    {0x00,0x60,0x60,0x00,0x00}, // '.'
    {0x20,0x10,0x08,0x04,0x02}, // '/'
    {0x3E,0x51,0x49,0x45,0x3E}, // '0'
    {0x00,0x42,0x7F,0x40,0x00}, // '1'
    {0x42,0x61,0x51,0x49,0x46}, // '2'
    {0x21,0x41,0x45,0x4B,0x31}, // '3'
    {0x18,0x14,0x12,0x7F,0x10}, // '4'
    {0x27,0x45,0x45,0x45,0x39}, // '5'
    {0x3C,0x4A,0x49,0x49,0x30}, // '6'
    {0x01,0x71,0x09,0x05,0x03}, // '7'
    {0x36,0x49,0x49,0x49,0x36}, // '8'
    {0x06,0x49,0x49,0x29,0x1E}, // '9'
    {0x00,0x36,0x36,0x00,0x00}, // ':'
    {0x00,0x56,0x36,0x00,0x00}, // ';'
    {0x00,0x08,0x14,0x22,0x41}, // '<'
    {0x14,0x14,0x14,0x14,0x14}, // '='
    {0x41,0x22,0x14,0x08,0x00}, // '>'
    {0x02,0x01,0x51,0x09,0x06}, // '?'
    {0x32,0x49,0x79,0x41,0x3E}, // '@'
    {0x7E,0x11,0x11,0x11,0x7E}, // 'A'
    {0x7F,0x49,0x49,0x49,0x36}, // 'B'
    {0x3E,0x41,0x41,0x41,0x22}, // 'C'
    {0x7F,0x41,0x41,0x22,0x1C}, // 'D'
    {0x7F,0x49,0x49,0x49,0x41}, // 'E'
    {0x7F,0x09,0x09,0x09,0x01}, // 'F'
    {0x3E,0x41,0x49,0x49,0x7A}, // 'G'
    {0x7F,0x08,0x08,0x08,0x7F}, // 'H'
    {0x00,0x41,0x7F,0x41,0x00}, // 'I'
    {0x20,0x40,0x41,0x3F,0x01}, // 'J'
    {0x7F,0x08,0x14,0x22,0x41}, // 'K'
    {0x7F,0x40,0x40,0x40,0x40}, // 'L'
    {0x7F,0x02,0x04,0x02,0x7F}, // 'M'
    {0x7F,0x04,0x08,0x10,0x7F}, // 'N'
    {0x3E,0x41,0x41,0x41,0x3E}, // 'O'
    {0x7F,0x09,0x09,0x09,0x06}, // 'P'
    {0x3E,0x41,0x51,0x21,0x5E}, // 'Q'
    {0x7F,0x09,0x19,0x29,0x46}, // 'R'
    {0x46,0x49,0x49,0x49,0x31}, // 'S'
    {0x01,0x01,0x7F,0x01,0x01}, // 'T'
    {0x3F,0x40,0x40,0x40,0x3F}, // 'U'
    {0x1F,0x20,0x40,0x20,0x1F}, // 'V'
    {0x3F,0x40,0x38,0x40,0x3F}, // 'W'
    {0x63,0x14,0x08,0x14,0x63}, // 'X'
    {0x07,0x08,0x70,0x08,0x07}, // 'Y'
    {0x61,0x51,0x49,0x45,0x43}, // 'Z'
    {0x00,0x7F,0x41,0x41,0x00}, // '['
    {0x02,0x04,0x08,0x10,0x20}, // '\\'
    {0x00,0x41,0x41,0x7F,0x00}, // ']'
    {0x04,0x02,0x01,0x02,0x04}, // '^'
    {0x40,0x40,0x40,0x40,0x40}, // '_'
    {0x00,0x01,0x02,0x04,0x00}, // '`'
    {0x20,0x54,0x54,0x54,0x78}, // 'a'
    {0x7F,0x48,0x44,0x44,0x38}, // 'b'
    {0x38,0x44,0x44,0x44,0x20}, // 'c'
    {0x38,0x44,0x44,0x48,0x7F}, // 'd'
    {0x38,0x54,0x54,0x54,0x18}, // 'e'
    {0x08,0x7E,0x09,0x01,0x02}, // 'f'
    {0x08,0x14,0x54,0x54,0x3C}, // 'g'
    {0x7F,0x08,0x04,0x04,0x78}, // 'h'
    {0x00,0x44,0x7D,0x40,0x00}, // 'i'
    {0x20,0x40,0x44,0x3D,0x00}, // 'j'
    {0x7F,0x10,0x28,0x44,0x00}, // 'k'
    {0x00,0x41,0x7F,0x40,0x00}, // 'l'
    {0x7C,0x04,0x18,0x04,0x78}, // 'm'
    {0x7C,0x08,0x04,0x04,0x78}, // 'n'
    {0x38,0x44,0x44,0x44,0x38}, // 'o'
    {0x7C,0x14,0x14,0x14,0x08}, // 'p'
    {0x08,0x14,0x14,0x18,0x7C}, // 'q'
    {0x7C,0x08,0x04,0x04,0x08}, // 'r'
    {0x48,0x54,0x54,0x54,0x20}, // 's'
    {0x04,0x3F,0x44,0x40,0x20}, // 't'
    {0x3C,0x40,0x40,0x40,0x7C}, // 'u'
    {0x1C,0x20,0x40,0x20,0x1C}, // 'v'
    {0x3C,0x40,0x30,0x40,0x3C}, // 'w'
    {0x44,0x28,0x10,0x28,0x44}, // 'x'
    {0x0C,0x50,0x50,0x50,0x3C}, // 'y'
    {0x44,0x64,0x54,0x4C,0x44}, // 'z'
    {0x00,0x08,0x36,0x41,0x00}, // '{'
    {0x00,0x00,0x7F,0x00,0x00}, // '|'
    {0x00,0x41,0x36,0x08,0x00}, // '}'
    {0x08,0x04,0x08,0x10,0x08}, // '~'
};

void UnitLcd::drawChar(uint8_t x, uint8_t y, char ch, uint16_t fg, uint16_t bg, uint8_t scale) {
    if (!dev_ || scale == 0) return;
    if (ch < 32 || ch > 126) ch = '?';
    const uint8_t* glyph = FONT5X7[ch - 32];
    uint16_t W = width(), H = height();
    for (uint8_t col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (uint8_t row = 0; row < 7; row++) {
            uint16_t color = (bits & (1u << row)) ? fg : bg;
            uint16_t px = (uint16_t)x + col * scale;
            uint16_t py = (uint16_t)y + row * scale;
            if (px >= W || py >= H) continue;
            if (scale == 1) {
                drawPixel((uint8_t)px, (uint8_t)py, color);
            } else {
                uint16_t px1 = std::min((uint16_t)(px + scale - 1), (uint16_t)(W - 1));
                uint16_t py1 = std::min((uint16_t)(py + scale - 1), (uint16_t)(H - 1));
                fillRect((uint8_t)px, (uint8_t)py, (uint8_t)px1, (uint8_t)py1, color);
            }
        }
    }
    // Trailing gap column in background colour
    uint16_t gx = (uint16_t)x + 5 * scale;
    if (gx < W) {
        uint16_t gx1 = std::min((uint16_t)(gx + scale - 1), (uint16_t)(W - 1));
        uint16_t gy1 = std::min((uint16_t)(y + 7 * scale - 1), (uint16_t)(H - 1));
        fillRect(gx, y, (uint8_t)gx1, (uint8_t)gy1, bg);
    }
}

void UnitLcd::drawText(uint8_t x, uint8_t y, const char* str, uint16_t fg, uint16_t bg, uint8_t scale) {
    if (!dev_ || !str) return;
    uint8_t cx = x;
    uint16_t charWidth = 6 * scale;
    while (*str) {
        if (cx + charWidth > width()) break;  // Stop if next char would be off-screen
        drawChar(cx, y, *str++, fg, bg, scale);
        cx += charWidth;
    }
}
