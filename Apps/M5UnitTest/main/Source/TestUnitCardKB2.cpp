#include "TestUnitCardKB2.h"
#include "M5UnitTest.h"
#include "GroveLookup.h"
#include "UiScale.h"
#include <tactility/device.h>
#include <tactility/drivers/uart_controller.h>
#include <lvgl_window_manager/window_manager.h>
#include <lvgl/fonts.h>
#include <cstring>

namespace {

// ---------------------------------------------------------------------------
// Physical key layout - 5 rows (4 real + arrow row).
// matchChar=0 = modifier key (no highlight target).
// Arrow keys produced by Fn+Z/X/D/C in I2C mode; by key-id in UART mode.
// ---------------------------------------------------------------------------
const struct { const char* label; uint8_t matchChar; int grow; } LAYOUT[][12] = {
    // Row 0: 1 2 3 4 5 6 7 8 9 0 Del  (+ Esc shown but Esc = Fn+1, no direct key-id)
    { {"1",'1',1},{"2",'2',1},{"3",'3',1},{"4",'4',1},{"5",'5',1},{"6",'6',1},
      {"7",'7',1},{"8",'8',1},{"9",'9',1},{"0",'0',1},{"Del",0x08,1},{nullptr,0,0} },
    // Row 1: q w e r t y u i o p Del
    { {"q",'q',1},{"w",'w',1},{"e",'e',1},{"r",'r',1},{"t",'t',1},{"y",'y',1},
      {"u",'u',1},{"i",'i',1},{"o",'o',1},{"p",'p',1},{"Del",0x08,1},{nullptr,0,0} },
    // Row 2: Aa a s d f g h j k l Enter
    { {"Aa",0,1},{"a",'a',1},{"s",'s',1},{"d",'d',1},{"f",'f',1},{"g",'g',1},
      {"h",'h',1},{"j",'j',1},{"k",'k',1},{"l",'l',1},{"Ent",0x0A,1},{nullptr,0,0} },
    // Row 3: Fn Sym z x c v b n m Spc
    { {"Fn",0,1},{"Sym",0,1},{"z",'z',1},{"x",'x',1},{"c",'c',1},{"v",'v',1},
      {"b",'b',1},{"n",'n',1},{"m",'m',1},{"Spc",0x20,3},{nullptr,0,0},{nullptr,0,0} },
    // Row 4: arrow keys (ASCII from Fn combos in I2C; produced by Fn key-ids in UART)
    { {"<",0x1D,1},{"v",0x1F,1},{"^",0x1E,1},{">",0x1C,1},
      {nullptr,0,0},{nullptr,0,0},{nullptr,0,0},{nullptr,0,0},{nullptr,0,0},{nullptr,0,0},{nullptr,0,0},{nullptr,0,0} },
};
constexpr int ROW_COUNT = 5;
constexpr int COL_COUNT = 12;

void connectI2C(lv_event_t* e);
void connectUart(lv_event_t* e);

// ---------------------------------------------------------------------------
// Grid construction
// ---------------------------------------------------------------------------

void buildGrid(TestUnitCardKB2* self, lv_obj_t* parent) {
    self->gridCount_ = 0;
    for (int row = 0; row < ROW_COUNT; row++) {
        lv_obj_t* rowCont = lv_obj_create(parent);
        lv_obj_set_width(rowCont, LV_PCT(100));
        lv_obj_set_height(rowCont, LV_SIZE_CONTENT);
        lv_obj_set_layout(rowCont, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(rowCont, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(rowCont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(rowCont, uiPad() / 4 + 1, 0);
        lv_obj_set_style_pad_all(rowCont, 0, 0);
        lv_obj_set_style_bg_opa(rowCont, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(rowCont, 0, 0);

        for (int col = 0; col < COL_COUNT; col++) {
            if (LAYOUT[row][col].label == nullptr) break;
            lv_obj_t* btn = lv_button_create(rowCont);
            lv_obj_set_style_pad_hor(btn, uiPad() / 2, 0);
            lv_obj_set_style_pad_ver(btn, uiRowGap(), 0);
            lv_obj_set_style_radius(btn, 4, 0);
            lv_obj_set_flex_grow(btn, LAYOUT[row][col].grow);

            lv_obj_t* lbl = lv_label_create(btn);
            lv_obj_set_style_text_font(lbl, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
            lv_label_set_text(lbl, LAYOUT[row][col].label);
            lv_obj_center(lbl);

            if (self->gridCount_ < TestUnitCardKB2::GRID_KEY_COUNT) {
                self->grid_[self->gridCount_++] = { LAYOUT[row][col].label, LAYOUT[row][col].matchChar, btn, lbl };
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Connection overlay
// ---------------------------------------------------------------------------

void showConnectOverlay(TestUnitCardKB2* self) {
    int pad    = uiPad();
    int rowGap = uiRowGap();
    const lv_font_t* fnt = lvgl_get_text_font(uiFont());

    self->connectOverlay_ = lv_obj_create(self->parentRef_);
    lv_obj_set_size(self->connectOverlay_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(self->connectOverlay_, 0, 0);
    lv_obj_set_layout(self->connectOverlay_, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(self->connectOverlay_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(self->connectOverlay_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(self->connectOverlay_, pad, 0);
    lv_obj_set_style_pad_row(self->connectOverlay_, rowGap * 2, 0);

    lv_obj_t* title = lv_label_create(self->connectOverlay_);
    lv_obj_set_style_text_font(title, fnt, 0);
    lv_label_set_text(title, "Select connection mode");

    lv_obj_t* hint = lv_label_create(self->connectOverlay_);
    lv_obj_set_style_text_font(hint, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x888888), 0);
    lv_label_set_text(hint, "Fn+Sym+1 = I2C  |  Fn+Sym+2 = UART");

    auto makeBtn = [&](const char* label, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_button_create(self->connectOverlay_);
        lv_obj_set_width(btn, LV_PCT(60));
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, self);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, fnt, 0);
        lv_label_set_text(lbl, label);
        lv_obj_center(lbl);
    };
    makeBtn("I2C (Grove port)", connectI2C);
    makeBtn("UART (Grove port)", connectUart);
}

// ---------------------------------------------------------------------------
// Main content UI (built after connection type selected)
// ---------------------------------------------------------------------------

void buildMainUI(TestUnitCardKB2* self) {
    memset(self->history_, 0, sizeof(self->history_));
    self->histLen_    = 0;
    self->gridCount_  = 0;
    self->activeBtn_  = nullptr;

    lv_obj_t* cont = lv_obj_create(self->parentRef_);
    lv_obj_set_width(cont, LV_PCT(100));
    lv_obj_set_flex_grow(cont, 1);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(cont, 3, 0);
    lv_obj_set_style_pad_all(cont, uiPad(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);

    if (uiW() >= 200) buildGrid(self, cont);

    self->lblHistory_ = lv_label_create(cont);
    lv_obj_set_style_text_font(self->lblHistory_, lvgl_get_text_font(uiFont()), 0);
    lv_label_set_text(self->lblHistory_, "");
    lv_obj_set_width(self->lblHistory_, LV_PCT(100));
    lv_label_set_long_mode(self->lblHistory_, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(self->lblHistory_, 1);
}

// ---------------------------------------------------------------------------
// PaHub helper
// ---------------------------------------------------------------------------

void selectIfNeeded(TestUnitCardKB2* self) {
    if (self->usingPaHub_ && self->hub_.isPresent())
        self->hub_.select(self->hub_.currentChannel());
}

// ---------------------------------------------------------------------------
// Update (called from timer)
// ---------------------------------------------------------------------------

void update(TestUnitCardKB2* self) {
    if (!self->unit_.isPresent()) return;
    if (self->unit_.mode() == UnitCardKB2::Mode::I2C) selectIfNeeded(self);

    char c = self->unit_.getKey();

    // Grid highlight
    if (self->gridCount_ > 0) {
        lv_obj_t* newActive = nullptr;
        if (c != 0) {
            for (int i = 0; i < self->gridCount_; i++) {
                if (self->grid_[i].matchChar == 0 || self->grid_[i].btn == nullptr) continue;
                uint8_t mc = self->grid_[i].matchChar;
                // Match lower and uppercase variants of letter keys
                bool match = (mc == (uint8_t)c) ||
                             (mc >= 'a' && mc <= 'z' && mc == (uint8_t)(c | 0x20));
                if (match) { newActive = self->grid_[i].btn; break; }
            }
        }
        if (newActive != self->activeBtn_) {
            if (self->activeBtn_) lv_obj_remove_state(self->activeBtn_, LV_STATE_PRESSED);
            if (newActive)  lv_obj_add_state(newActive, LV_STATE_PRESSED);
            self->activeBtn_ = newActive;
        }
    }

    // History strip - printable chars only
    if (c != 0 && c >= 0x20 && c < 0x7F) {
        if (self->histLen_ < sizeof(self->history_) - 1) {
            self->history_[self->histLen_++] = c;
        } else {
            memmove(self->history_, self->history_ + 1, self->histLen_ - 1);
            self->history_[self->histLen_ - 1] = c;
        }
        self->history_[self->histLen_] = '\0';
        lv_label_set_text(self->lblHistory_, self->history_);
    }
}

void onTimer(lv_timer_t* t) {
    auto* self = static_cast<TestUnitCardKB2*>(lv_timer_get_user_data(t));
    if (window_manager_get_state(self->app_->window) != WINDOW_STATE_GRANTED) return;
    update(self);
}

// ---------------------------------------------------------------------------
// Connect handlers
// ---------------------------------------------------------------------------

void doConnectI2C(TestUnitCardKB2* self) {
    lv_obj_delete(self->connectOverlay_);
    self->connectOverlay_ = nullptr;

    Device* i2c = findGroveI2cDevice();
    if (!i2c) {
        buildMainUI(self);
        lv_label_set_text(self->lblHistory_, "grove0_i2c not found");
        return;
    }

    bool ok = false;
    if (self->unit_.begin(i2c)) {
        self->usingPaHub_ = false;
        ok = true;
    } else if (self->hub_.begin(i2c)) {
        self->usingPaHub_ = true;
        for (uint8_t ch = 0; ch < UnitPaHub::NUM_CHANNELS && !ok; ch++) {
            self->hub_.select(ch);
            if (self->unit_.begin(i2c)) ok = true;
        }
        if (!ok) self->hub_.deselect();
    }

    buildMainUI(self);
    if (!ok) lv_label_set_text(self->lblHistory_, "CardKB2 not found");
    else     self->timer_ = lv_timer_create(onTimer, 50, self);
}

void doConnectUart(TestUnitCardKB2* self) {
    lv_obj_delete(self->connectOverlay_);
    self->connectOverlay_ = nullptr;

    Device* uart = findGroveUartDevice();
    buildMainUI(self);
    if (!uart) {
        lv_label_set_text(self->lblHistory_, "grove0_uart not found");
        return;
    }
    if (!self->unit_.beginUart(uart)) {
        lv_label_set_text(self->lblHistory_, "UART open failed");
        return;
    }
    self->timer_ = lv_timer_create(onTimer, 50, self);
}

void connectI2C(lv_event_t* e) {
    doConnectI2C(static_cast<TestUnitCardKB2*>(lv_event_get_user_data(e)));
}

void connectUart(lv_event_t* e) {
    doConnectUart(static_cast<TestUnitCardKB2*>(lv_event_get_user_data(e)));
}

} // namespace

void testUnitCardKB2Start(TestUnitCardKB2* self, lv_obj_t* parent, Context* app) {
    self->app_       = app;
    self->parentRef_ = parent;

    testViewCreateToolbar(parent, app, "CardKB2");
    testViewCreateBanner(parent, "CardKB2", "I2C/UART", COLOR_I2C);

    showConnectOverlay(self);
}

void testUnitCardKB2Stop(TestUnitCardKB2* self) {
    if (self->timer_) { lv_timer_delete(self->timer_); self->timer_ = nullptr; }
    if (self->usingPaHub_ && self->hub_.isPresent()) self->hub_.deselect();
    self->unit_.end();
    self->lblHistory_     = nullptr;
    self->connectOverlay_ = nullptr;
    self->activeBtn_      = nullptr;
    self->gridCount_      = 0;
}
