#include "TestUnitLcdGfx.h"
#include "UiScale.h"
#include <tactility/device.h>
#include <tactility/lvgl_fonts.h>
#include <esp_timer.h>
#include <cstring>
#include <cstdio>
#include <algorithm>

static inline uint32_t usNow() {
    return (uint32_t)esp_timer_get_time();
}

static const char* PHASE_NAMES[] = {
    "Screen fill",
    "Text",
    "Pixels",
    "Lines",
    "H/V Lines",
    "Rectangles (filled)",
    "Rectangles (outline)",
    "Triangles (filled)",
    "Triangles (outline)",
    "Circles (filled)",
    "Circles (outline)",
    "Arcs (filled)",
    "Arcs (outline)",
    "Round rects (filled)",
    "Round rects (outline)",
    "Results",
};
static constexpr int PHASE_COUNT = (int)(sizeof(PHASE_NAMES) / sizeof(PHASE_NAMES[0]));

// ---------------------------------------------------------------------------

void TestUnitLcdGfx::onStart(lv_obj_t* parent, AppHandle handle, M5UnitTest* app) {
    app_        = app;
    phase_      = 0;
    logBuf_[0]  = '\0';
    memset(results_, 0, sizeof(results_));

    createToolbar(parent, handle, "LCD Gfx Test");
    createBanner(parent, "LCD Gfx", "I2C", COLOR_I2C);

    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_width(cont, LV_PCT(100));
    lv_obj_set_flex_grow(cont, 1);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, uiPad(), 0);
    lv_obj_set_style_pad_row(cont, uiRowGap(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);

    const lv_font_t* fnt  = lvgl_get_text_font(uiFont());
    const lv_font_t* fntS = lvgl_get_text_font(FONT_SIZE_SMALL);

    lblPhase_ = lv_label_create(cont);
    lv_obj_set_style_text_font(lblPhase_, fnt, 0);
    lv_label_set_text(lblPhase_, "Searching...");

    lblLog_ = lv_label_create(cont);
    lv_obj_set_style_text_font(lblLog_, fntS, 0);
    lv_label_set_long_mode(lblLog_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lblLog_, LV_PCT(100));
    lv_label_set_text(lblLog_, "");

    Device* i2c = device_find_by_name("i2c1");
    if (!i2c) { lv_label_set_text(lblPhase_, "i2c1 not found"); return; }

    if (lcd_.begin(i2c)) {
        usingPaHub_ = false;
    } else if (hub_.begin(i2c)) {
        usingPaHub_ = true;
        bool found = false;
        for (uint8_t ch = 0; ch < UnitPaHub::NUM_CHANNELS && !found; ch++) {
            hub_.select(ch);
            if (lcd_.begin(i2c)) found = true;
        }
        if (!found) {
            hub_.deselect();
            lv_label_set_text(lblPhase_, "LCD not found");
            return;
        }
    } else {
        lv_label_set_text(lblPhase_, "LCD not found");
        return;
    }

    lcd_.setBrightness(180);
    lcd_.setRotation(0);

    // Pre-compute layout constants
    w_     = (int16_t)lcd_.width();
    h_     = (int16_t)lcd_.height();
    minDim_ = std::min(w_, h_);
    minDim1_= minDim_ - 1;
    cx_    = w_ / 2;
    cy_    = h_ / 2;
    cx1_   = cx_ - 1;
    cy1_   = cy_ - 1;
    cMin_  = std::min(cx1_, cy1_);
    cMin1_ = cMin_ - 1;

    lv_label_set_text(lblPhase_, PHASE_NAMES[0]);
    timer_ = lv_timer_create(onTimer, 200, this);
    lv_timer_set_repeat_count(timer_, 1);
}

void TestUnitLcdGfx::onStop() {
    if (timer_) { lv_timer_delete(timer_); timer_ = nullptr; }
    selectIfNeeded();
    if (lcd_.isPresent()) lcd_.setBrightness(0);
    if (usingPaHub_ && hub_.isPresent()) hub_.deselect();
    lblPhase_ = lblLog_ = nullptr;
}

void TestUnitLcdGfx::selectIfNeeded() {
    if (usingPaHub_ && hub_.isPresent())
        hub_.select(hub_.currentChannel());
}

void TestUnitLcdGfx::onTimer(lv_timer_t* t) {
    static_cast<TestUnitLcdGfx*>(lv_timer_get_user_data(t))->runNextPhase();
}

void TestUnitLcdGfx::appendLog(const char* name, uint32_t us) {
    size_t len = strlen(logBuf_);
    size_t rem = sizeof(logBuf_) - len;
    snprintf(logBuf_ + len, rem, "%-20s %lu\n", name, (unsigned long)us);
    lv_label_set_text(lblLog_, logBuf_);
}

void TestUnitLcdGfx::runNextPhase() {
    timer_ = nullptr;
    if (!lcd_.isPresent()) return;
    selectIfNeeded();

    if (phase_ >= PHASE_COUNT) { lv_label_set_text(lblPhase_, "Done!"); return; }

    lv_label_set_text(lblPhase_, PHASE_NAMES[phase_]);

    if (phase_ < PHASE_COUNT - 1) {
        // Benchmark phase
        uint32_t us = 0;
        switch (phase_) {
            case 0:  us = testFillScreen();        break;
            case 1:  us = testText();              break;
            case 2:  us = testPixels();            break;
            case 3:  us = testLines();             break;
            case 4:  us = testFastLines();         break;
            case 5:  us = testFilledRects();       break;
            case 6:  us = testRects();             break;
            case 7:  us = testFilledTriangles();   break;
            case 8:  us = testTriangles();         break;
            case 9:  us = testFilledCircles();     break;
            case 10: us = testCircles();           break;
            case 11: us = testFillArcs();          break;
            case 12: us = testArcs();              break;
            case 13: us = testFilledRoundRects();  break;
            case 14: us = testRoundRects();        break;
        }
        results_[phase_] = us;
        appendLog(PHASE_NAMES[phase_], us);
    } else {
        // Results screen on the LCD itself
        drawResultsOnLcd();
        lv_label_set_text(lblPhase_, "Done!");
        phase_++;
        return;
    }

    phase_++;
    // Short pause between phases so the LCD buffer drains
    timer_ = lv_timer_create(onTimer, 80, this);
    lv_timer_set_repeat_count(timer_, 1);
}

// Draw the timing summary on the LCD unit itself, matching PDQ's results screen.
// PDQ background: c cycles 4..11 and is used directly as an RGB565 value
// (these are near-black blues: 0x0004..0x000B). The subtle banding effect is
// identical to the Arduino PDQ sketch's final loop.
void TestUnitLcdGfx::drawResultsOnLcd() {
    uint16_t cyan    = UnitLcd::rgb888to565(0x00FFFF);
    uint16_t yellow  = UnitLcd::rgb888to565(0xFFFF00);
    uint16_t green   = UnitLcd::rgb888to565(0x00FF00);
    uint16_t magenta = UnitLcd::rgb888to565(0xFF00FF);

    uint16_t W = lcd_.width(), H = lcd_.height();

    // PDQ blue-band background - c is a raw RGB565 value cycling 4..11
    {
        uint16_t c = 4;
        int8_t   d = 1;
        for (uint16_t i = 0; i < H; i++) {
            lcd_.drawHLine(0, (uint8_t)i, (uint8_t)W, c);
            c = (uint16_t)(c + d);
            if (c <= 4 || c >= 11) d = -d;
        }
    }

    // Title - "LCD GFX PDQ" in magenta (PDQ uses "Arduino GFX PDQ")
    uint8_t y = 2;
    lcd_.drawText(2, y, "LCD GFX PDQ", magenta, 0x0006, 1); y += 10;

    // Header line - green, matching PDQ's "\nBenchmark  micro-secs"
    lcd_.drawText(2, y, "Benchmark   micro-secs", green, 0x0006, 1); y += 10;

    // Results - cyan label + yellow number, one per row, 9px line height
    // Names padded to 12 chars; number right-aligned in 9 chars (matches PDQ comma style)
    static const char* SHORT_NAMES[] = {
        "Screen fill",
        "Text       ",
        "Pixels     ",
        "Lines      ",
        "H/V Lines  ",
        "Rectangles F",
        "Rectangles ",
        "Triangles F",
        "Triangles  ",
        "Circles F  ",
        "Circles    ",
        "Arcs F     ",
        "Arcs       ",
        "RoundRects F",
        "RoundRects ",
    };
    for (int i = 0; i < RESULT_COUNT; i++) {
        if ((int)y + 9 > (int)H - 9) break;
        // Label in cyan
        lcd_.drawText(2, y, SHORT_NAMES[i], cyan, 0x0006, 1);
        // Number in yellow, formatted with commas like PDQ's printnice()
        char num[14];
        snprintf(num, sizeof(num), "%lu", (unsigned long)results_[i]);
        // Insert commas right-to-left (PDQ style)
        for (char* p = (num + strlen(num)) - 3; p > num; p -= 3) {
            memmove(p + 1, p, strlen(p) + 1);
            *p = ',';
        }
        // Right-align in the remaining width (screen is 135px, label ~72px, 63px left)
        // Draw at fixed x so numbers line up
        lcd_.drawText(74, y, num, yellow, 0x0006, 1);
        y += 9;
    }

    lcd_.drawText(2, (uint8_t)(H - 9), "Benchmark Complete!", green, 0x0006, 1);
}

// ---------------------------------------------------------------------------
// Benchmark phases - matching PDQ exactly
// ---------------------------------------------------------------------------

uint32_t TestUnitLcdGfx::testFillScreen() {
    uint32_t s = usNow();
    lcd_.fillScreen(0xFFFF);
    lcd_.fillScreen(0xF800);
    lcd_.fillScreen(0x07E0);
    lcd_.fillScreen(0x001F);
    lcd_.fillScreen(0x0000);
    return usNow() - s;
}

uint32_t TestUnitLcdGfx::testText() {
    // Mirror PDQ testText() - for a 135px wide screen tsa/tsb/tsc all = 1.
    // Scale 2 is used once at the end (fits: "Size 2" = 6 chars × 12px = 72px).
    uint16_t black = 0x0000;
    lcd_.fillScreen(black);
    uint32_t s = usNow();
    uint8_t y = 0;
    lcd_.drawText(0, y, "Hello World!",        0xFFFF, black, 1); y += 9;
    lcd_.drawText(0, y, "RED GREEN BLUE",      UnitLcd::color565(255,0,0),   black, 1); y += 9;
    lcd_.drawText(0, y, "1234.56",             UnitLcd::color565(255,255,0), black, 1); y += 9;
    lcd_.drawText(0, y, "0xDEADBEEF",          0xFFFF, black, 1); y += 9;
    lcd_.drawText(0, y, "Groop,",              UnitLcd::color565(0,255,255), black, 1); y += 9;
    lcd_.drawText(0, y, "I implore thee,",     UnitLcd::color565(255,0,255), black, 1); y += 9;
    lcd_.drawText(0, y, "my foonting",         UnitLcd::color565(0,0,200),   black, 1); y += 9;
    lcd_.drawText(0, y, "turlingdromes.",       UnitLcd::color565(0,128,0),   black, 1); y += 9;
    lcd_.drawText(0, y, "crinkly bindlewurdles",UnitLcd::color565(0,128,128), black, 1); y += 9;
    lcd_.drawText(0, y, "Or I will rend thee", UnitLcd::color565(128,0,0),   black, 1); y += 9;
    lcd_.drawText(0, y, "gobberwartsb",         UnitLcd::color565(128,0,128), black, 1); y += 9;
    lcd_.drawText(0, y, "blurglecruncheon,",    UnitLcd::color565(128,128,0), black, 1); y += 9;
    lcd_.drawText(0, y, "see if I don't!",      UnitLcd::color565(64,64,64),  black, 1); y += 9;
    lcd_.drawText(0, y, "Size 2",              UnitLcd::color565(255,0,0),   black, 2); y += 18;
    lcd_.drawText(0, y, "Size 3",              UnitLcd::color565(255,165,0), black, 2); // capped at 2
    return usNow() - s;
}

uint32_t TestUnitLcdGfx::testPixels() {
    lcd_.fillScreen(0x0000);
    uint32_t s = usNow();
    for (int16_t y = 0; y < h_; y++) {
        for (int16_t x = 0; x < w_; x++) {
            lcd_.drawPixel((uint8_t)x, (uint8_t)y,
                UnitLcd::color565((uint8_t)(x << 3), (uint8_t)(y << 3),
                                  (uint8_t)((x * y) & 0xFF)));
        }
    }
    return usNow() - s;
}

uint32_t TestUnitLcdGfx::testLines() {
    uint16_t blue = 0x001F;
    lcd_.fillScreen(0x0000);
    uint32_t s = usNow();
    // All 4 corners x 2 sweeps each (matching PDQ exactly)
    for (int16_t x = 0; x < w_; x += 6) lcd_.drawLine(0,      0,      x,    h_-1, blue);
    for (int16_t y = 0; y < h_; y += 6) lcd_.drawLine(0,      0,      w_-1, y,    blue);
    lcd_.fillScreen(0x0000);
    for (int16_t x = 0; x < w_; x += 6) lcd_.drawLine(w_-1,   0,      x,    h_-1, blue);
    for (int16_t y = 0; y < h_; y += 6) lcd_.drawLine(w_-1,   0,      0,    y,    blue);
    lcd_.fillScreen(0x0000);
    for (int16_t x = 0; x < w_; x += 6) lcd_.drawLine(0,      h_-1,   x,    0,    blue);
    for (int16_t y = 0; y < h_; y += 6) lcd_.drawLine(0,      h_-1,   w_-1, y,    blue);
    lcd_.fillScreen(0x0000);
    for (int16_t x = 0; x < w_; x += 6) lcd_.drawLine(w_-1,   h_-1,   x,    0,    blue);
    for (int16_t y = 0; y < h_; y += 6) lcd_.drawLine(w_-1,   h_-1,   0,    y,    blue);
    return usNow() - s;
}

uint32_t TestUnitLcdGfx::testFastLines() {
    lcd_.fillScreen(0x0000);
    uint32_t s = usNow();
    for (int16_t y = 0; y < h_; y += 5) lcd_.drawHLine(0, (uint8_t)y, (uint8_t)w_, 0xF800);
    for (int16_t x = 0; x < w_; x += 5) lcd_.drawVLine((uint8_t)x, 0, (uint8_t)h_, 0x001F);
    return usNow() - s;
}

uint32_t TestUnitLcdGfx::testFilledRects() {
    lcd_.fillScreen(0x0000);
    uint32_t s = usNow();
    for (int16_t i = minDim_; i > 0; i -= 6) {
        int16_t half = i / 2;
        lcd_.fillRect((uint8_t)(cx_ - half), (uint8_t)(cy_ - half),
                      (uint8_t)(cx_ + half - 1), (uint8_t)(cy_ + half - 1),
                      UnitLcd::color565((uint8_t)std::min((int)i, 255),
                                        (uint8_t)std::min((int)i, 255), 0));
    }
    return usNow() - s;
}

uint32_t TestUnitLcdGfx::testRects() {
    // Don't clear - runs on top of filled rects (matches PDQ)
    uint32_t s = usNow();
    for (int16_t i = 2; i < minDim_; i += 6) {
        int16_t half = i / 2;
        lcd_.drawRect((uint8_t)(cx_ - half), (uint8_t)(cy_ - half),
                      (uint8_t)i, (uint8_t)i, 0x07E0);
    }
    return usNow() - s;
}

uint32_t TestUnitLcdGfx::testFilledTriangles() {
    lcd_.fillScreen(0x0000);
    uint32_t s = usNow();
    for (int16_t i = cMin1_; i > 10; i -= 5) {
        lcd_.fillTriangle(cx1_, cy1_ - i, cx1_ - i, cy1_ + i, cx1_ + i, cy1_ + i,
            UnitLcd::color565(0, (uint8_t)std::min(i*2, 255), (uint8_t)std::min(i*2, 255)));
    }
    return usNow() - s;
}

uint32_t TestUnitLcdGfx::testTriangles() {
    // Don't clear - runs on top (matches PDQ)
    uint32_t s = usNow();
    for (int16_t i = 0; i < cMin_; i += 5) {
        lcd_.drawTriangle(cx1_, cy1_ - i, cx1_ - i, cy1_ + i, cx1_ + i, cy1_ + i,
            UnitLcd::color565(0, 0, (uint8_t)std::min(i*4, 255)));
    }
    return usNow() - s;
}

uint32_t TestUnitLcdGfx::testFilledCircles() {
    lcd_.fillScreen(0x0000);
    uint32_t s = usNow();
    for (int16_t x = 10; x < (int16_t)w_; x += 20)
        for (int16_t y = 10; y < (int16_t)h_; y += 20)
            lcd_.fillCircle(x, y, 10, 0xF81F);
    return usNow() - s;
}

uint32_t TestUnitLcdGfx::testCircles() {
    // Don't clear (matches PDQ)
    uint32_t s = usNow();
    for (int16_t x = 0; x <= (int16_t)w_ + 10; x += 20)
        for (int16_t y = 0; y <= (int16_t)h_ + 10; y += 20)
            lcd_.drawCircle(x, y, 10, 0xFFFF);
    return usNow() - s;
}

uint32_t TestUnitLcdGfx::testFillArcs() {
    lcd_.fillScreen(0x0000);
    int16_t r = (cMin_ > 0) ? (360 / cMin_) : 6;
    uint32_t s = usNow();
    for (int16_t i = 6; i < cMin_; i += 6)
        lcd_.fillArc(cx1_, cy1_, i, i - 3, 0.0f, (float)(i * r), 0xF800);
    return usNow() - s;
}

uint32_t TestUnitLcdGfx::testArcs() {
    // Don't clear (matches PDQ)
    int16_t r = (cMin_ > 0) ? (360 / cMin_) : 6;
    uint32_t s = usNow();
    for (int16_t i = 6; i < cMin_; i += 6)
        lcd_.drawArc(cx1_, cy1_, i, i - 3, 0.0f, (float)(i * r), 0xFFFF);
    return usNow() - s;
}

uint32_t TestUnitLcdGfx::testFilledRoundRects() {
    lcd_.fillScreen(0x0000);
    uint32_t s = usNow();
    for (int16_t i = minDim1_; i > 20; i -= 6) {
        int16_t half = i / 2;
        lcd_.fillRoundRect(cx_ - half, cy_ - half, i, i, i / 8,
            UnitLcd::color565(0, (uint8_t)std::min(i*2, 255), 0));
    }
    return usNow() - s;
}

uint32_t TestUnitLcdGfx::testRoundRects() {
    // Don't clear (matches PDQ)
    uint32_t s = usNow();
    for (int16_t i = 20; i < minDim1_; i += 6) {
        int16_t half = i / 2;
        lcd_.drawRoundRect(cx_ - half, cy_ - half, i, i, i / 8,
            UnitLcd::color565((uint8_t)std::min(i*2, 255), 0, 0));
    }
    return usNow() - s;
}
