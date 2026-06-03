#include "TestUnitDualButton.h"
#include "UiScale.h"
#include <tactility/device.h>
#include <tactility/lvgl_fonts.h>

static constexpr gpio_pin_t PIN_MIN = 0;
static constexpr gpio_pin_t PIN_MAX = 57;

static constexpr lv_color_t COLOR_A_ACTIVE  = LV_COLOR_MAKE(0xE0, 0x30, 0x30);
static constexpr lv_color_t COLOR_A_DIM     = LV_COLOR_MAKE(0x40, 0x10, 0x10);
static constexpr lv_color_t COLOR_B_ACTIVE  = LV_COLOR_MAKE(0x30, 0x60, 0xE0);
static constexpr lv_color_t COLOR_B_DIM     = LV_COLOR_MAKE(0x10, 0x20, 0x50);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Scale UI elements off the shorter display side so config looks good in both orientations.
static lv_coord_t uiShort() {
    lv_coord_t w = lv_display_get_horizontal_resolution(nullptr);
    lv_coord_t h = lv_display_get_vertical_resolution(nullptr);
    return w < h ? w : h;
}

// Button size and value label width proportional to short side, clamped to reasonable range.
static lv_coord_t uiBtnSize()  { lv_coord_t s = uiShort() / 10; return s < 36 ? 36 : (s > 80 ? 80 : s); }
static lv_coord_t uiValWidth() { lv_coord_t s = uiShort() / 8;  return s < 48 ? 48 : (s > 100 ? 100 : s); }

static lv_obj_t* makePinRow(lv_obj_t* parent, const char* label,
                             lv_event_cb_t cbDown, lv_event_cb_t cbUp,
                             lv_obj_t** outLbl, void* userData) {
    int pad = uiPad();
    int gap = uiRowGap() * 2;
    lv_coord_t btnSz  = uiBtnSize();
    lv_coord_t valW   = uiValWidth();
    // Use LARGE font on larger screens (short side >= 400), DEFAULT otherwise
    enum LvglFontSize fnt = uiShort() >= 400 ? FONT_SIZE_LARGE : FONT_SIZE_DEFAULT;

    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, gap, 0);
    lv_obj_set_style_pad_ver(row, pad, 0);
    lv_obj_set_style_pad_hor(row, pad, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);

    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, lvgl_get_text_font(fnt), 0);

    lv_obj_t* btnMinus = lv_button_create(row);
    lv_obj_set_size(btnMinus, btnSz, btnSz);
    lv_obj_add_event_cb(btnMinus, cbDown, LV_EVENT_CLICKED, userData);
    lv_obj_t* lblMinus = lv_label_create(btnMinus);
    lv_obj_set_style_text_font(lblMinus, lvgl_get_text_font(fnt), 0);
    lv_label_set_text(lblMinus, "-");
    lv_obj_center(lblMinus);

    lv_obj_t* valLbl = lv_label_create(row);
    lv_obj_set_style_text_font(valLbl, lvgl_get_text_font(fnt), 0);
    lv_obj_set_width(valLbl, valW);
    lv_obj_set_style_text_align(valLbl, LV_TEXT_ALIGN_CENTER, 0);
    *outLbl = valLbl;

    lv_obj_t* btnPlus = lv_button_create(row);
    lv_obj_set_size(btnPlus, btnSz, btnSz);
    lv_obj_add_event_cb(btnPlus, cbUp, LV_EVENT_CLICKED, userData);
    lv_obj_t* lblPlus = lv_label_create(btnPlus);
    lv_obj_set_style_text_font(lblPlus, lvgl_get_text_font(fnt), 0);
    lv_label_set_text(lblPlus, "+");
    lv_obj_center(lblPlus);

    return row;
}

// ---------------------------------------------------------------------------
// onStart
// ---------------------------------------------------------------------------

void TestUnitDualButton::onStart(lv_obj_t* parent, AppHandle handle, M5UnitTest* app) {
    app_ = app;
    createToolbar(parent, handle, "Dual-Button");
    createBanner(parent, "Dual-Button", "GPIO", COLOR_GPIO);
    buildConfigScreen(parent);
}

void TestUnitDualButton::buildConfigScreen(lv_obj_t* parent) {
    int pad = uiPad();
    int gap = uiRowGap();

    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_width(cont, LV_PCT(100));
    lv_obj_set_flex_grow(cont, 1);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cont, gap, 0);
    lv_obj_set_style_pad_all(cont, pad, 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);

    makePinRow(cont, "Pin A:", onPinADown, onPinAUp, &lblPinA_, this);
    lv_label_set_text_fmt(lblPinA_, "%d", (int)pinA_);

    makePinRow(cont, "Pin B:", onPinBDown, onPinBUp, &lblPinB_, this);
    lv_label_set_text_fmt(lblPinB_, "%d", (int)pinB_);

    enum LvglFontSize fnt = uiShort() >= 400 ? FONT_SIZE_LARGE : FONT_SIZE_DEFAULT;
    lv_coord_t btnH = uiBtnSize() * 3 / 2;

    lv_obj_t* btnConnect = lv_button_create(cont);
    lv_obj_set_width(btnConnect, LV_PCT(60));
    lv_obj_set_height(btnConnect, btnH);
    lv_obj_add_event_cb(btnConnect, onConnect, LV_EVENT_CLICKED, this);
    lv_obj_t* lbl = lv_label_create(btnConnect);
    lv_obj_set_style_text_font(lbl, lvgl_get_text_font(fnt), 0);
    lv_label_set_text(lbl, "Connect");
    lv_obj_center(lbl);

    lblError_ = lv_label_create(cont);
    lv_obj_set_style_text_color(lblError_, lv_color_make(0xE0, 0x40, 0x40), 0);
    lv_obj_set_style_text_font(lblError_, lvgl_get_text_font(fnt), 0);
    lv_label_set_text(lblError_, "");
}

void TestUnitDualButton::buildTestScreen(lv_obj_t* parent) {
    // Remove config screen (last child added - the cont with flex_grow=1)
    // We clean the parent area by deleting children after toolbar+banner (first 2),
    // so instead we track via the cont pointer approach: just clean parent and rebuild
    // toolbar+banner are already created; we delete everything after them by cleaning
    // what was added by buildConfigScreen. The simplest approach: delete the cont we
    // created. Since we don't store it, clean children from index 2 onward.
    uint32_t childCnt = lv_obj_get_child_count(parent);
    for (uint32_t i = childCnt; i > 2; --i) {
        lv_obj_delete(lv_obj_get_child(parent, i - 1));
    }
    lblPinA_ = lblPinB_ = lblError_ = nullptr;

    int pad = uiPad();

    lv_coord_t dispW = lv_display_get_horizontal_resolution(nullptr);
    lv_coord_t dispH = lv_display_get_vertical_resolution(nullptr);
    bool landscape = dispW > dispH;

    // Diameter: fit two circles side-by-side in landscape, stacked in portrait.
    // Base off the shorter display dimension to avoid blowing up on wide screens.
    lv_coord_t shortSide = dispW < dispH ? dispW : dispH;
    lv_coord_t overhead  = 40 + 28 + pad * 2;  // toolbar + banner + padding
    lv_coord_t diam;
    if (landscape) {
        // Side by side: diameter limited by available height and half the width
        lv_coord_t byH = dispH - overhead - pad * 2;
        lv_coord_t byW = (dispW - pad * 3) / 2;
        diam = byH < byW ? byH : byW;
    } else {
        // Stacked: diameter limited by width and half available height
        lv_coord_t byW = dispW - pad * 2;
        lv_coord_t byH = (dispH - overhead - pad * 3) / 2;
        diam = byW < byH ? byW : byH;
    }
    // Cap at 40% of short side so circles never dominate the whole screen
    lv_coord_t cap = shortSide * 2 / 5;
    if (diam > cap) diam = cap;
    if (diam < 20)  diam = 20;

    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_width(cont, LV_PCT(100));
    lv_obj_set_flex_grow(cont, 1);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, landscape ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(cont, pad, 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);

    // Circle A (red)
    circleA_ = lv_obj_create(cont);
    lv_obj_set_size(circleA_, diam, diam);
    lv_obj_set_style_radius(circleA_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circleA_, COLOR_A_DIM, 0);
    lv_obj_set_style_bg_opa(circleA_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(circleA_, 0, 0);
    lv_obj_set_style_pad_all(circleA_, 0, 0);

    circleLblA_ = lv_label_create(circleA_);
    lv_label_set_text(circleLblA_, "A");
    lv_obj_set_style_text_font(circleLblA_, lvgl_get_text_font(FONT_SIZE_LARGE), 0);
    lv_obj_set_style_text_color(circleLblA_, lv_color_make(0x80, 0x40, 0x40), 0);
    lv_obj_set_style_text_align(circleLblA_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(circleLblA_, LV_ALIGN_CENTER, 0, 0);

    // Circle B (blue)
    circleB_ = lv_obj_create(cont);
    lv_obj_set_size(circleB_, diam, diam);
    lv_obj_set_style_radius(circleB_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circleB_, COLOR_B_DIM, 0);
    lv_obj_set_style_bg_opa(circleB_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(circleB_, 0, 0);
    lv_obj_set_style_pad_all(circleB_, 0, 0);

    circleLblB_ = lv_label_create(circleB_);
    lv_label_set_text(circleLblB_, "B");
    lv_obj_set_style_text_font(circleLblB_, lvgl_get_text_font(FONT_SIZE_LARGE), 0);
    lv_obj_set_style_text_color(circleLblB_, lv_color_make(0x40, 0x50, 0x80), 0);
    lv_obj_set_style_text_align(circleLblB_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(circleLblB_, LV_ALIGN_CENTER, 0, 0);

    timer_ = lv_timer_create(onTimer, 50, this);
    update();
}

// ---------------------------------------------------------------------------
// onStop
// ---------------------------------------------------------------------------

void TestUnitDualButton::onStop() {
    if (timer_) { lv_timer_delete(timer_); timer_ = nullptr; }
    if (connected_) { unit_.end(); connected_ = false; }
    lblPinA_ = lblPinB_ = lblError_ = nullptr;
    circleA_ = circleB_ = nullptr;
    circleLblA_ = circleLblB_ = nullptr;
    pinA_ = 0;
    pinB_ = 49;
}

// ---------------------------------------------------------------------------
// update (polls hardware, refreshes circles)
// ---------------------------------------------------------------------------

void TestUnitDualButton::update() {
    if (!unit_.isPresent() || !circleA_) return;

    bool pressA = unit_.isButtonAPressed();
    lv_obj_set_style_bg_color(circleA_, pressA ? COLOR_A_ACTIVE : COLOR_A_DIM, 0);
    lv_label_set_text(circleLblA_, pressA ? "PRESSED" : "A");
    lv_obj_set_style_text_color(circleLblA_,
        pressA ? lv_color_white() : lv_color_make(0x80, 0x40, 0x40), 0);

    bool pressB = unit_.isButtonBPressed();
    lv_obj_set_style_bg_color(circleB_, pressB ? COLOR_B_ACTIVE : COLOR_B_DIM, 0);
    lv_label_set_text(circleLblB_, pressB ? "PRESSED" : "B");
    lv_obj_set_style_text_color(circleLblB_,
        pressB ? lv_color_white() : lv_color_make(0x40, 0x50, 0x80), 0);
}

// ---------------------------------------------------------------------------
// Static callbacks
// ---------------------------------------------------------------------------

void TestUnitDualButton::onTimer(lv_timer_t* t) {
    static_cast<TestUnitDualButton*>(lv_timer_get_user_data(t))->update();
}

void TestUnitDualButton::onPinADown(lv_event_t* e) {
    auto* self = static_cast<TestUnitDualButton*>(lv_event_get_user_data(e));
    if (self->pinA_ > PIN_MIN) {
        self->pinA_--;
        lv_label_set_text_fmt(self->lblPinA_, "%d", (int)self->pinA_);
    }
}

void TestUnitDualButton::onPinAUp(lv_event_t* e) {
    auto* self = static_cast<TestUnitDualButton*>(lv_event_get_user_data(e));
    if (self->pinA_ < PIN_MAX) {
        self->pinA_++;
        lv_label_set_text_fmt(self->lblPinA_, "%d", (int)self->pinA_);
    }
}

void TestUnitDualButton::onPinBDown(lv_event_t* e) {
    auto* self = static_cast<TestUnitDualButton*>(lv_event_get_user_data(e));
    if (self->pinB_ > PIN_MIN) {
        self->pinB_--;
        lv_label_set_text_fmt(self->lblPinB_, "%d", (int)self->pinB_);
    }
}

void TestUnitDualButton::onPinBUp(lv_event_t* e) {
    auto* self = static_cast<TestUnitDualButton*>(lv_event_get_user_data(e));
    if (self->pinB_ < PIN_MAX) {
        self->pinB_++;
        lv_label_set_text_fmt(self->lblPinB_, "%d", (int)self->pinB_);
    }
}

void TestUnitDualButton::onConnect(lv_event_t* e) {
    auto* self = static_cast<TestUnitDualButton*>(lv_event_get_user_data(e));
    Device* gpio = device_find_by_name("gpio0");
    if (!gpio || !self->unit_.begin(gpio, self->pinA_, self->pinB_)) {
        if (self->lblError_) {
            lv_label_set_text(self->lblError_, "GPIO init failed - check pins");
        }
        return;
    }
    self->connected_ = true;
    // Obtain the parent (grandparent of the button's container)
    lv_obj_t* btn = lv_event_get_target_obj(e);
    lv_obj_t* cont = lv_obj_get_parent(btn);
    lv_obj_t* parent = lv_obj_get_parent(cont);
    self->buildTestScreen(parent);
}
