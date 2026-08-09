#include "TestUnitLcdGfx.h"
#include "M5UnitTest.h"
#include "GroveLookup.h"
#include "UiScale.h"
#include <tactility/device.h>
#include <lvgl_window_manager/window_manager.h>
#include <lvgl/fonts.h>
#include <esp_timer.h>
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace {

uint32_t usNow() {
    return (uint32_t)esp_timer_get_time();
}

const char* PHASE_NAMES[] = {
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
constexpr int PHASE_COUNT = (int)(sizeof(PHASE_NAMES) / sizeof(PHASE_NAMES[0]));

// ---------------------------------------------------------------------------
// Benchmark phases - matching PDQ exactly
// ---------------------------------------------------------------------------

uint32_t testFillScreen(TestUnitLcdGfx* self) {
    uint32_t s = usNow();
    self->lcd_.fillScreen(0xFFFF);
    self->lcd_.fillScreen(0xF800);
    self->lcd_.fillScreen(0x07E0);
    self->lcd_.fillScreen(0x001F);
    self->lcd_.fillScreen(0x0000);
    return usNow() - s;
}

uint32_t testText(TestUnitLcdGfx* self) {
    // Mirror PDQ testText() - for a 135px wide screen tsa/tsb/tsc all = 1.
    // Scale 2 is used once at the end (fits: "Size 2" = 6 chars × 12px = 72px).
    uint16_t black = 0x0000;
    self->lcd_.fillScreen(black);
    uint32_t s = usNow();
    uint8_t y = 0;
    self->lcd_.drawText(0, y, "Hello World!",        0xFFFF, black, 1); y += 9;
    self->lcd_.drawText(0, y, "RED GREEN BLUE",      UnitLcd::color565(255,0,0),   black, 1); y += 9;
    self->lcd_.drawText(0, y, "1234.56",             UnitLcd::color565(255,255,0), black, 1); y += 9;
    self->lcd_.drawText(0, y, "0xDEADBEEF",          0xFFFF, black, 1); y += 9;
    self->lcd_.drawText(0, y, "Groop,",              UnitLcd::color565(0,255,255), black, 1); y += 9;
    self->lcd_.drawText(0, y, "I implore thee,",     UnitLcd::color565(255,0,255), black, 1); y += 9;
    self->lcd_.drawText(0, y, "my foonting",         UnitLcd::color565(0,0,200),   black, 1); y += 9;
    self->lcd_.drawText(0, y, "turlingdromes.",       UnitLcd::color565(0,128,0),   black, 1); y += 9;
    self->lcd_.drawText(0, y, "crinkly bindlewurdles",UnitLcd::color565(0,128,128), black, 1); y += 9;
    self->lcd_.drawText(0, y, "Or I will rend thee", UnitLcd::color565(128,0,0),   black, 1); y += 9;
    self->lcd_.drawText(0, y, "gobberwartsb",         UnitLcd::color565(128,0,128), black, 1); y += 9;
    self->lcd_.drawText(0, y, "blurglecruncheon,",    UnitLcd::color565(128,128,0), black, 1); y += 9;
    self->lcd_.drawText(0, y, "see if I don't!",      UnitLcd::color565(64,64,64),  black, 1); y += 9;
    self->lcd_.drawText(0, y, "Size 2",              UnitLcd::color565(255,0,0),   black, 2); y += 18;
    self->lcd_.drawText(0, y, "Size 3",              UnitLcd::color565(255,165,0), black, 2); // capped at 2
    return usNow() - s;
}

uint32_t testPixels(TestUnitLcdGfx* self) {
    self->lcd_.fillScreen(0x0000);
    uint32_t s = usNow();
    for (int16_t y = 0; y < self->h_; y++) {
        for (int16_t x = 0; x < self->w_; x++) {
            self->lcd_.drawPixel((uint8_t)x, (uint8_t)y,
                UnitLcd::color565((uint8_t)(x << 3), (uint8_t)(y << 3),
                                  (uint8_t)((x * y) & 0xFF)));
        }
    }
    return usNow() - s;
}

uint32_t testLines(TestUnitLcdGfx* self) {
    uint16_t blue = 0x001F;
    int16_t w_ = self->w_, h_ = self->h_;
    self->lcd_.fillScreen(0x0000);
    uint32_t s = usNow();
    // All 4 corners x 2 sweeps each (matching PDQ exactly)
    for (int16_t x = 0; x < w_; x += 6) self->lcd_.drawLine(0,      0,      x,    h_-1, blue);
    for (int16_t y = 0; y < h_; y += 6) self->lcd_.drawLine(0,      0,      w_-1, y,    blue);
    self->lcd_.fillScreen(0x0000);
    for (int16_t x = 0; x < w_; x += 6) self->lcd_.drawLine(w_-1,   0,      x,    h_-1, blue);
    for (int16_t y = 0; y < h_; y += 6) self->lcd_.drawLine(w_-1,   0,      0,    y,    blue);
    self->lcd_.fillScreen(0x0000);
    for (int16_t x = 0; x < w_; x += 6) self->lcd_.drawLine(0,      h_-1,   x,    0,    blue);
    for (int16_t y = 0; y < h_; y += 6) self->lcd_.drawLine(0,      h_-1,   w_-1, y,    blue);
    self->lcd_.fillScreen(0x0000);
    for (int16_t x = 0; x < w_; x += 6) self->lcd_.drawLine(w_-1,   h_-1,   x,    0,    blue);
    for (int16_t y = 0; y < h_; y += 6) self->lcd_.drawLine(w_-1,   h_-1,   0,    y,    blue);
    return usNow() - s;
}

uint32_t testFastLines(TestUnitLcdGfx* self) {
    self->lcd_.fillScreen(0x0000);
    uint32_t s = usNow();
    for (int16_t y = 0; y < self->h_; y += 5) self->lcd_.drawHLine(0, (uint8_t)y, (uint8_t)self->w_, 0xF800);
    for (int16_t x = 0; x < self->w_; x += 5) self->lcd_.drawVLine((uint8_t)x, 0, (uint8_t)self->h_, 0x001F);
    return usNow() - s;
}

uint32_t testFilledRects(TestUnitLcdGfx* self) {
    self->lcd_.fillScreen(0x0000);
    uint32_t s = usNow();
    for (int16_t i = self->minDim_; i > 0; i -= 6) {
        int16_t half = i / 2;
        self->lcd_.fillRect((uint8_t)(self->cx_ - half), (uint8_t)(self->cy_ - half),
                      (uint8_t)(self->cx_ + half - 1), (uint8_t)(self->cy_ + half - 1),
                      UnitLcd::color565((uint8_t)std::min((int)i, 255),
                                        (uint8_t)std::min((int)i, 255), 0));
    }
    return usNow() - s;
}

uint32_t testRects(TestUnitLcdGfx* self) {
    // Don't clear - runs on top of filled rects (matches PDQ)
    uint32_t s = usNow();
    for (int16_t i = 2; i < self->minDim_; i += 6) {
        int16_t half = i / 2;
        self->lcd_.drawRect((uint8_t)(self->cx_ - half), (uint8_t)(self->cy_ - half),
                      (uint8_t)i, (uint8_t)i, 0x07E0);
    }
    return usNow() - s;
}

uint32_t testFilledTriangles(TestUnitLcdGfx* self) {
    self->lcd_.fillScreen(0x0000);
    uint32_t s = usNow();
    for (int16_t i = self->cMin1_; i > 10; i -= 5) {
        self->lcd_.fillTriangle(self->cx1_, self->cy1_ - i, self->cx1_ - i, self->cy1_ + i, self->cx1_ + i, self->cy1_ + i,
            UnitLcd::color565(0, (uint8_t)std::min(i*2, 255), (uint8_t)std::min(i*2, 255)));
    }
    return usNow() - s;
}

uint32_t testTriangles(TestUnitLcdGfx* self) {
    // Don't clear - runs on top (matches PDQ)
    uint32_t s = usNow();
    for (int16_t i = 0; i < self->cMin_; i += 5) {
        self->lcd_.drawTriangle(self->cx1_, self->cy1_ - i, self->cx1_ - i, self->cy1_ + i, self->cx1_ + i, self->cy1_ + i,
            UnitLcd::color565(0, 0, (uint8_t)std::min(i*4, 255)));
    }
    return usNow() - s;
}

uint32_t testFilledCircles(TestUnitLcdGfx* self) {
    self->lcd_.fillScreen(0x0000);
    uint32_t s = usNow();
    for (int16_t x = 10; x < (int16_t)self->w_; x += 20)
        for (int16_t y = 10; y < (int16_t)self->h_; y += 20)
            self->lcd_.fillCircle(x, y, 10, 0xF81F);
    return usNow() - s;
}

uint32_t testCircles(TestUnitLcdGfx* self) {
    // Don't clear (matches PDQ)
    uint32_t s = usNow();
    for (int16_t x = 0; x <= (int16_t)self->w_ + 10; x += 20)
        for (int16_t y = 0; y <= (int16_t)self->h_ + 10; y += 20)
            self->lcd_.drawCircle(x, y, 10, 0xFFFF);
    return usNow() - s;
}

uint32_t testFillArcs(TestUnitLcdGfx* self) {
    self->lcd_.fillScreen(0x0000);
    int16_t r = (self->cMin_ > 0) ? (360 / self->cMin_) : 6;
    uint32_t s = usNow();
    for (int16_t i = 6; i < self->cMin_; i += 6)
        self->lcd_.fillArc(self->cx1_, self->cy1_, i, i - 3, 0.0f, (float)(i * r), 0xF800);
    return usNow() - s;
}

uint32_t testArcs(TestUnitLcdGfx* self) {
    // Don't clear (matches PDQ)
    int16_t r = (self->cMin_ > 0) ? (360 / self->cMin_) : 6;
    uint32_t s = usNow();
    for (int16_t i = 6; i < self->cMin_; i += 6)
        self->lcd_.drawArc(self->cx1_, self->cy1_, i, i - 3, 0.0f, (float)(i * r), 0xFFFF);
    return usNow() - s;
}

uint32_t testFilledRoundRects(TestUnitLcdGfx* self) {
    self->lcd_.fillScreen(0x0000);
    uint32_t s = usNow();
    for (int16_t i = self->minDim1_; i > 20; i -= 6) {
        int16_t half = i / 2;
        self->lcd_.fillRoundRect(self->cx_ - half, self->cy_ - half, i, i, i / 8,
            UnitLcd::color565(0, (uint8_t)std::min(i*2, 255), 0));
    }
    return usNow() - s;
}

uint32_t testRoundRects(TestUnitLcdGfx* self) {
    // Don't clear (matches PDQ)
    uint32_t s = usNow();
    for (int16_t i = 20; i < self->minDim1_; i += 6) {
        int16_t half = i / 2;
        self->lcd_.drawRoundRect(self->cx_ - half, self->cy_ - half, i, i, i / 8,
            UnitLcd::color565((uint8_t)std::min(i*2, 255), 0, 0));
    }
    return usNow() - s;
}

// ---------------------------------------------------------------------------

void selectIfNeeded(TestUnitLcdGfx* self) {
    if (self->usingPaHub_ && self->hub_.isPresent())
        self->hub_.select(self->hub_.currentChannel());
}

void appendLog(TestUnitLcdGfx* self, const char* name, uint32_t us) {
    size_t len = strlen(self->logBuf_);
    size_t rem = sizeof(self->logBuf_) - len;
    snprintf(self->logBuf_ + len, rem, "%-20s %lu\n", name, (unsigned long)us);
    lv_label_set_text(self->lblLog_, self->logBuf_);
}

// Draw the timing summary on the LCD unit itself, matching PDQ's results screen.
// PDQ background: c cycles 4..11 and is used directly as an RGB565 value
// (these are near-black blues: 0x0004..0x000B). The subtle banding effect is
// identical to the Arduino PDQ sketch's final loop.
void drawResultsOnLcd(TestUnitLcdGfx* self) {
    uint16_t cyan    = UnitLcd::rgb888to565(0x00FFFF);
    uint16_t yellow  = UnitLcd::rgb888to565(0xFFFF00);
    uint16_t green   = UnitLcd::rgb888to565(0x00FF00);
    uint16_t magenta = UnitLcd::rgb888to565(0xFF00FF);

    uint16_t W = self->lcd_.width(), H = self->lcd_.height();

    // PDQ blue-band background - c is a raw RGB565 value cycling 4..11
    {
        uint16_t c = 4;
        int8_t   d = 1;
        for (uint16_t i = 0; i < H; i++) {
            self->lcd_.drawHLine(0, (uint8_t)i, (uint8_t)W, c);
            c = (uint16_t)(c + d);
            if (c <= 4 || c >= 11) d = -d;
        }
    }

    // Title - "LCD GFX PDQ" in magenta (PDQ uses "Arduino GFX PDQ")
    uint8_t y = 2;
    self->lcd_.drawText(2, y, "LCD GFX PDQ", magenta, 0x0006, 1); y += 10;

    // Header line - green, matching PDQ's "\nBenchmark  micro-secs"
    self->lcd_.drawText(2, y, "Benchmark   micro-secs", green, 0x0006, 1); y += 10;

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
    for (int i = 0; i < TestUnitLcdGfx::RESULT_COUNT; i++) {
        if ((int)y + 9 > (int)H - 9) break;
        // Label in cyan
        self->lcd_.drawText(2, y, SHORT_NAMES[i], cyan, 0x0006, 1);
        // Number in yellow, formatted with commas like PDQ's printnice()
        char num[14];
        snprintf(num, sizeof(num), "%lu", (unsigned long)self->results_[i]);
        // Insert commas right-to-left (PDQ style)
        for (char* p = (num + strlen(num)) - 3; p > num; p -= 3) {
            memmove(p + 1, p, strlen(p) + 1);
            *p = ',';
        }
        // Right-align in the remaining width (screen is 135px, label ~72px, 63px left)
        // Draw at fixed x so numbers line up
        self->lcd_.drawText(74, y, num, yellow, 0x0006, 1);
        y += 9;
    }

    self->lcd_.drawText(2, (uint8_t)(H - 9), "Benchmark Complete!", green, 0x0006, 1);
}

void onTimer(lv_timer_t* t);

void runNextPhase(TestUnitLcdGfx* self) {
    self->timer_ = nullptr;
    if (!self->lcd_.isPresent()) return;
    selectIfNeeded(self);

    if (self->phase_ >= PHASE_COUNT) { lv_label_set_text(self->lblPhase_, "Done!"); return; }

    lv_label_set_text(self->lblPhase_, PHASE_NAMES[self->phase_]);

    if (self->phase_ < PHASE_COUNT - 1) {
        // Benchmark phase
        uint32_t us = 0;
        switch (self->phase_) {
            case 0:  us = testFillScreen(self);        break;
            case 1:  us = testText(self);              break;
            case 2:  us = testPixels(self);             break;
            case 3:  us = testLines(self);              break;
            case 4:  us = testFastLines(self);          break;
            case 5:  us = testFilledRects(self);        break;
            case 6:  us = testRects(self);              break;
            case 7:  us = testFilledTriangles(self);    break;
            case 8:  us = testTriangles(self);          break;
            case 9:  us = testFilledCircles(self);      break;
            case 10: us = testCircles(self);            break;
            case 11: us = testFillArcs(self);           break;
            case 12: us = testArcs(self);               break;
            case 13: us = testFilledRoundRects(self);   break;
            case 14: us = testRoundRects(self);         break;
        }
        self->results_[self->phase_] = us;
        appendLog(self, PHASE_NAMES[self->phase_], us);
    } else {
        // Results screen on the LCD itself
        drawResultsOnLcd(self);
        lv_label_set_text(self->lblPhase_, "Done!");
        self->phase_++;
        return;
    }

    self->phase_++;
    // Short pause between phases so the LCD buffer drains
    self->timer_ = lv_timer_create(onTimer, 80, self);
    lv_timer_set_repeat_count(self->timer_, 1);
}

void onTimer(lv_timer_t* t) {
    auto* self = static_cast<TestUnitLcdGfx*>(lv_timer_get_user_data(t));
    if (window_manager_get_state(self->app_->window) != WINDOW_STATE_GRANTED) return;
    runNextPhase(self);
}

} // namespace

void testUnitLcdGfxStart(TestUnitLcdGfx* self, lv_obj_t* parent, Context* app) {
    self->app_        = app;
    self->phase_      = 0;
    self->logBuf_[0]  = '\0';
    memset(self->results_, 0, sizeof(self->results_));

    testViewCreateToolbar(parent, app, "LCD Gfx Test");
    testViewCreateBanner(parent, "LCD Gfx", "I2C", COLOR_I2C);

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

    self->lblPhase_ = lv_label_create(cont);
    lv_obj_set_style_text_font(self->lblPhase_, fnt, 0);
    lv_label_set_text(self->lblPhase_, "Searching...");

    self->lblLog_ = lv_label_create(cont);
    lv_obj_set_style_text_font(self->lblLog_, fntS, 0);
    lv_label_set_long_mode(self->lblLog_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(self->lblLog_, LV_PCT(100));
    lv_label_set_text(self->lblLog_, "");

    Device* i2c = findGroveI2cDevice();
    if (!i2c) { lv_label_set_text(self->lblPhase_, "grove0_i2c not found"); return; }

    if (self->lcd_.begin(i2c)) {
        self->usingPaHub_ = false;
    } else if (self->hub_.begin(i2c)) {
        self->usingPaHub_ = true;
        bool found = false;
        for (uint8_t ch = 0; ch < UnitPaHub::NUM_CHANNELS && !found; ch++) {
            self->hub_.select(ch);
            if (self->lcd_.begin(i2c)) found = true;
        }
        if (!found) {
            self->hub_.deselect();
            lv_label_set_text(self->lblPhase_, "LCD not found");
            return;
        }
    } else {
        lv_label_set_text(self->lblPhase_, "LCD not found");
        return;
    }

    self->lcd_.setBrightness(180);
    self->lcd_.setRotation(0);

    // Pre-compute layout constants
    self->w_     = (int16_t)self->lcd_.width();
    self->h_     = (int16_t)self->lcd_.height();
    self->minDim_ = std::min(self->w_, self->h_);
    self->minDim1_= self->minDim_ - 1;
    self->cx_    = self->w_ / 2;
    self->cy_    = self->h_ / 2;
    self->cx1_   = self->cx_ - 1;
    self->cy1_   = self->cy_ - 1;
    self->cMin_  = std::min(self->cx1_, self->cy1_);
    self->cMin1_ = self->cMin_ - 1;

    lv_label_set_text(self->lblPhase_, PHASE_NAMES[0]);
    self->timer_ = lv_timer_create(onTimer, 200, self);
    lv_timer_set_repeat_count(self->timer_, 1);
}

void testUnitLcdGfxStop(TestUnitLcdGfx* self) {
    if (self->timer_) { lv_timer_delete(self->timer_); self->timer_ = nullptr; }
    selectIfNeeded(self);
    if (self->lcd_.isPresent()) self->lcd_.setBrightness(0);
    if (self->usingPaHub_ && self->hub_.isPresent()) self->hub_.deselect();
    self->lblPhase_ = self->lblLog_ = nullptr;
}
