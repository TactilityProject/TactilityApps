#include "TestUnitCardKB2.h"
#include "UiScale.h"
#include <tactility/device.h>
#include <tactility/drivers/uart_controller.h>
#include <tactility/lvgl_fonts.h>
#include <cstring>

// ---------------------------------------------------------------------------
// Physical key layout - 5 rows (4 real + arrow row).
// matchChar=0 = modifier key (no highlight target).
// Arrow keys produced by Fn+Z/X/D/C in I2C mode; by key-id in UART mode.
// ---------------------------------------------------------------------------
static const struct { const char* label; uint8_t matchChar; int grow; } LAYOUT[][12] = {
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
static constexpr int ROW_COUNT = 5;
static constexpr int COL_COUNT = 12;

// ---------------------------------------------------------------------------
// Grid construction
// ---------------------------------------------------------------------------

void TestUnitCardKB2::buildGrid(lv_obj_t* parent) {
    gridCount_ = 0;
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

            if (gridCount_ < GRID_KEY_COUNT) {
                grid_[gridCount_++] = { LAYOUT[row][col].label, LAYOUT[row][col].matchChar, btn, lbl };
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Connection overlay
// ---------------------------------------------------------------------------

void TestUnitCardKB2::showConnectOverlay() {
    int pad    = uiPad();
    int rowGap = uiRowGap();
    const lv_font_t* fnt = lvgl_get_text_font(uiFont());

    connectOverlay_ = lv_obj_create(parentRef_);
    lv_obj_set_size(connectOverlay_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(connectOverlay_, 0, 0);
    lv_obj_set_layout(connectOverlay_, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(connectOverlay_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(connectOverlay_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(connectOverlay_, pad, 0);
    lv_obj_set_style_pad_row(connectOverlay_, rowGap * 2, 0);

    lv_obj_t* title = lv_label_create(connectOverlay_);
    lv_obj_set_style_text_font(title, fnt, 0);
    lv_label_set_text(title, "Select connection mode");

    lv_obj_t* hint = lv_label_create(connectOverlay_);
    lv_obj_set_style_text_font(hint, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x888888), 0);
    lv_label_set_text(hint, "Fn+Sym+1 = I2C  |  Fn+Sym+2 = UART");

    auto makeBtn = [&](const char* label, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_button_create(connectOverlay_);
        lv_obj_set_width(btn, LV_PCT(60));
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, this);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, fnt, 0);
        lv_label_set_text(lbl, label);
        lv_obj_center(lbl);
    };
    makeBtn("I2C (Grove port)", onI2CBtn);
    makeBtn("UART (Grove port)", onUartBtn);
}

// ---------------------------------------------------------------------------
// Connect handlers
// ---------------------------------------------------------------------------

void TestUnitCardKB2::connectI2C() {
    lv_obj_delete(connectOverlay_);
    connectOverlay_ = nullptr;

    Device* i2c = device_find_by_name("i2c1");
    if (!i2c) {
        buildMainUI();
        lv_label_set_text(lblHistory_, "i2c1 not found");
        return;
    }

    bool ok = false;
    if (unit_.begin(i2c)) {
        usingPaHub_ = false;
        ok = true;
    } else if (hub_.begin(i2c)) {
        usingPaHub_ = true;
        for (uint8_t ch = 0; ch < UnitPaHub::NUM_CHANNELS && !ok; ch++) {
            hub_.select(ch);
            if (unit_.begin(i2c)) ok = true;
        }
        if (!ok) hub_.deselect();
    }

    buildMainUI();
    if (!ok) lv_label_set_text(lblHistory_, "CardKB2 not found");
    else     timer_ = lv_timer_create(onTimer, 50, this);
}

void TestUnitCardKB2::connectUart() {
    lv_obj_delete(connectOverlay_);
    connectOverlay_ = nullptr;

    Device* uart = device_find_by_name("uart1");
    buildMainUI();
    if (!uart) {
        lv_label_set_text(lblHistory_, "uart1 not found");
        return;
    }
    if (!unit_.beginUart(uart)) {
        lv_label_set_text(lblHistory_, "UART open failed");
        return;
    }
    timer_ = lv_timer_create(onTimer, 50, this);
}

// ---------------------------------------------------------------------------
// Main content UI (built after connection type selected)
// ---------------------------------------------------------------------------

void TestUnitCardKB2::buildMainUI() {
    memset(history_, 0, sizeof(history_));
    histLen_    = 0;
    gridCount_  = 0;
    activeBtn_  = nullptr;

    lv_obj_t* cont = lv_obj_create(parentRef_);
    lv_obj_set_width(cont, LV_PCT(100));
    lv_obj_set_flex_grow(cont, 1);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(cont, 3, 0);
    lv_obj_set_style_pad_all(cont, uiPad(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);

    if (uiW() >= 200) buildGrid(cont);

    lblHistory_ = lv_label_create(cont);
    lv_obj_set_style_text_font(lblHistory_, lvgl_get_text_font(uiFont()), 0);
    lv_label_set_text(lblHistory_, "");
    lv_obj_set_width(lblHistory_, LV_PCT(100));
    lv_label_set_long_mode(lblHistory_, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(lblHistory_, 1);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void TestUnitCardKB2::onStart(lv_obj_t* parent, AppHandle handle, M5UnitTest* app) {
    app_       = app;
    parentRef_ = parent;
    handleRef_ = handle;

    createToolbar(parent, handle, "CardKB2");
    createBanner(parent, "CardKB2", "I2C/UART", COLOR_I2C);

    showConnectOverlay();
}

void TestUnitCardKB2::onStop() {
    if (timer_) { lv_timer_delete(timer_); timer_ = nullptr; }
    if (usingPaHub_ && hub_.isPresent()) hub_.deselect();
    unit_.end();
    lblHistory_     = nullptr;
    connectOverlay_ = nullptr;
    activeBtn_      = nullptr;
    gridCount_      = 0;
}

// ---------------------------------------------------------------------------
// Static callbacks
// ---------------------------------------------------------------------------

void TestUnitCardKB2::onI2CBtn(lv_event_t* e) {
    static_cast<TestUnitCardKB2*>(lv_event_get_user_data(e))->connectI2C();
}

void TestUnitCardKB2::onUartBtn(lv_event_t* e) {
    static_cast<TestUnitCardKB2*>(lv_event_get_user_data(e))->connectUart();
}

void TestUnitCardKB2::onTimer(lv_timer_t* t) {
    static_cast<TestUnitCardKB2*>(lv_timer_get_user_data(t))->update();
}

// ---------------------------------------------------------------------------
// PaHub helper
// ---------------------------------------------------------------------------

void TestUnitCardKB2::selectIfNeeded() {
    if (usingPaHub_ && hub_.isPresent())
        hub_.select(hub_.currentChannel());
}

// ---------------------------------------------------------------------------
// Update (called from timer)
// ---------------------------------------------------------------------------

void TestUnitCardKB2::update() {
    if (!unit_.isPresent()) return;
    if (unit_.mode() == UnitCardKB2::Mode::I2C) selectIfNeeded();

    char c = unit_.getKey();

    // Grid highlight
    if (gridCount_ > 0) {
        lv_obj_t* newActive = nullptr;
        if (c != 0) {
            for (int i = 0; i < gridCount_; i++) {
                if (grid_[i].matchChar == 0 || grid_[i].btn == nullptr) continue;
                uint8_t mc = grid_[i].matchChar;
                // Match lower and uppercase variants of letter keys
                bool match = (mc == (uint8_t)c) ||
                             (mc >= 'a' && mc <= 'z' && mc == (uint8_t)(c | 0x20));
                if (match) { newActive = grid_[i].btn; break; }
            }
        }
        if (newActive != activeBtn_) {
            if (activeBtn_) lv_obj_remove_state(activeBtn_, LV_STATE_PRESSED);
            if (newActive)  lv_obj_add_state(newActive, LV_STATE_PRESSED);
            activeBtn_ = newActive;
        }
    }

    // History strip - printable chars only
    if (c != 0 && c >= 0x20 && c < 0x7F) {
        if (histLen_ < sizeof(history_) - 1) {
            history_[histLen_++] = c;
        } else {
            memmove(history_, history_ + 1, histLen_ - 1);
            history_[histLen_ - 1] = c;
        }
        history_[histLen_] = '\0';
        lv_label_set_text(lblHistory_, history_);
    }
}
