#include "TestUnitPaHub.h"
#include "M5UnitTest.h"
#include "GroveLookup.h"
#include "UiScale.h"
#include <tactility/device.h>
#include <lvgl/fonts.h>
#include <esp_log.h>
#include <cstring>
#include <cstdio>

namespace {

// All known addresses now use an identity read: firmware version, LCD ID, or
// the MFRC522 VersionReg. The old write-only probes for 0x28/0x5F could ACK on
// an empty mux channel, so they are no longer treated as present.
bool isIdentityVerified(uint8_t addr) {
    switch (addr) {
        case 0x41: // 8Encoder: firmware version
        case 0x3E: // LCD: 4-byte ID
        case 0x40: // Scroll: firmware version
        case 0x47: // ByteButton: firmware version
        case 0x63: // Joystick2: firmware version
        case 0x28: // RFID2: MFRC522/WS1850S VersionReg
        case 0x5F: // CardKB2: firmware version
            return true;
        default:
            return false;
    }
}

void probeSelected(TestUnitPaHub* self, bool logResults) {
    if (!self->hub_.isPresent() || self->selChannel_ < 0 || self->selChannel_ >= TestUnitPaHub::CH_COUNT) return;

    if (!self->hub_.select((uint8_t)self->selChannel_)) {
        lv_label_set_text_fmt(self->lblCh_[self->selChannel_], "CH%d: select failed", self->selChannel_);
        lv_label_set_text_fmt(self->lblStatus_, "Probed CH%d", self->selChannel_);
        self->hub_.deselect();
        return;
    }

    // Some downstream STM32/ESP32 units need longer than the mux settling time
    // before their I2C listener answers again. A readback of the mux register
    // here can also be blocked by downstream clock stretching and trigger a bus
    // reset, which then makes the identity probes fail. Let the channel settle
    // and go straight to the identity reads.
    vTaskDelay(pdMS_TO_TICKS(20));

    // Probe known unit addresses only (full-range scan can wedge the ESP-IDF i2c_master bus FSM)
    Device* i2c = findGroveI2cDevice();
    if (!i2c) {
        lv_label_set_text_fmt(self->lblCh_[self->selChannel_], "CH%d: grove0_i2c not found", self->selChannel_);
        self->hub_.deselect();
        return;
    }

    auto scanAddresses = [&](char* found, bool& any) {
        strcpy(found, "Found: ");
        any = false;
        const uint8_t channel = (uint8_t)self->selChannel_;
        for (uint8_t addr : KNOWN_UNIT_ADDRS) {
            bool present = unitProbe(i2c, addr);
            if (!present) {
                // An absent address can leave the controller's bus state
                // unsettled. Reconnect the mux and give the real device one
                // more clean chance before deciding the address is empty.
                self->hub_.deselect();
                vTaskDelay(pdMS_TO_TICKS(5));
                if (self->hub_.select(channel)) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    present = unitProbe(i2c, addr);
                }
            }
            const bool verified = present && isIdentityVerified(addr);
            if (logResults) {
                ESP_LOGI("M5UnitTest", "PaHub CH%d 0x%02X: %s",
                         (int)self->selChannel_, (unsigned)addr,
                         verified ? "ACK (verified)" : "no");
            }
            if (verified) {
                size_t remaining = 256 - strlen(found) - 1;
                if (remaining < 7) {
                    strncat(found, "...", remaining);
                    break;
                }
                char hex[8];
                snprintf(hex, sizeof(hex), "0x%02X ", addr);
                strncat(found, hex, remaining);
                any = true;
            }
        }
        if (!any) strcpy(found, "No devices found");
    };

    char found[256];
    bool any = false;
    scanAddresses(found, any);

    // A failed probe can leave the controller's bus state unsettled. On a
    // complete miss, disconnect and re-select the channel, then try once more.
    // This is deliberately limited to the miss case so a populated channel
    // stays fast.
    if (!any) {
        ESP_LOGI("M5UnitTest", "PaHub CH%d: no devices, retrying after mux reset",
                 (int)self->selChannel_);
        self->hub_.deselect();
        vTaskDelay(pdMS_TO_TICKS(20));
        if (self->hub_.select((uint8_t)self->selChannel_)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            scanAddresses(found, any);
        }
    }

    if (logResults) {
        ESP_LOGI("M5UnitTest", "PaHub CH%d result: %s", (int)self->selChannel_, found);
    }

    lv_label_set_text_fmt(self->lblCh_[self->selChannel_], "CH%d: %s", self->selChannel_, found);
    lv_label_set_text_fmt(self->lblStatus_, "Probed CH%d", self->selChannel_);

    self->hub_.deselect();
}

void onChannelBtn(lv_event_t* e) {
    auto* self = static_cast<TestUnitPaHub*>(lv_event_get_user_data(e));
    int ch = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    self->selChannel_ = ch;
    ESP_LOGI("M5UnitTest", "Probing PaHub CH%d", ch);
    probeSelected(self, true);
}

} // namespace

void testUnitPaHubStart(TestUnitPaHub* self, lv_obj_t* parent, Context* app) {
    self->app_ = app;

    testViewCreateToolbar(parent, app, "PaHub");
    testViewCreateBanner(parent, "PaHub", "I2C", COLOR_I2C);

    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_width(cont, LV_PCT(100));
    lv_obj_set_flex_grow(cont, 1);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, uiPad(), 0);
    lv_obj_set_style_pad_row(cont, uiRowGap(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);

    const lv_font_t* fnt = lvgl_get_text_font(uiFont());
    int pad = uiPad();
    int gap = uiRowGap();

    // Row of channel buttons - flex-grow so they share space evenly at any width
    lv_obj_t* btnRow = lv_obj_create(cont);
    lv_obj_set_width(btnRow, LV_PCT(100));
    lv_obj_set_height(btnRow, LV_SIZE_CONTENT);
    lv_obj_set_layout(btnRow, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btnRow, gap, 0);
    lv_obj_set_style_bg_opa(btnRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_style_pad_all(btnRow, 0, 0);

    for (int i = 0; i < TestUnitPaHub::CH_COUNT; i++) {
        lv_obj_t* btn = lv_button_create(btnRow);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_style_pad_hor(btn, pad, 0);
        lv_obj_set_style_pad_ver(btn, gap, 0);
        lv_obj_set_user_data(btn, (void*)(intptr_t)i);
        lv_obj_add_event_cb(btn, onChannelBtn, LV_EVENT_CLICKED, self);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text_fmt(lbl, "CH%d", i);
        lv_obj_set_style_text_font(lbl, fnt, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        self->btnCh_[i] = btn;
    }

    self->lblStatus_ = lv_label_create(cont);
    lv_obj_set_style_text_font(self->lblStatus_, fnt, 0);
    lv_obj_set_width(self->lblStatus_, LV_PCT(100));
    lv_label_set_long_mode(self->lblStatus_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(self->lblStatus_, "Select a channel to probe");

    for (int i = 0; i < TestUnitPaHub::CH_COUNT; i++) {
        self->lblCh_[i] = lv_label_create(cont);
        lv_obj_set_style_text_font(self->lblCh_[i], fnt, 0);
        lv_obj_set_width(self->lblCh_[i], LV_PCT(100));
        lv_label_set_long_mode(self->lblCh_[i], LV_LABEL_LONG_WRAP);
        lv_label_set_text_fmt(self->lblCh_[i], "CH%d: -", i);
    }

    Device* i2c = findGroveI2cDevice();
    if (!i2c || !self->hub_.begin(i2c)) {
        lv_label_set_text(self->lblStatus_, "PaHub not found");
        return;
    }

    lv_label_set_text(self->lblStatus_, "PaHub ready - tap channel to probe");
}

void testUnitPaHubStop(TestUnitPaHub* self) {
    if (self->hub_.isPresent()) self->hub_.deselect();
    self->lblStatus_ = nullptr;
    for (int i = 0; i < TestUnitPaHub::CH_COUNT; i++) { self->btnCh_[i] = nullptr; self->lblCh_[i] = nullptr; }
}
