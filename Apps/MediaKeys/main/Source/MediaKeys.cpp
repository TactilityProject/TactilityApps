#include "MediaKeys.h"

#include <tactility/log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <tactility/lvgl_fonts.h>
#include <tt_lvgl.h>
#include <tt_lvgl_toolbar.h>

static const char* TAG = "MediaKeys";

// Button layout: two rows of three media-control buttons
static const char* BTN_MAP[] = {
    LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT, "\n",
    LV_SYMBOL_MUTE, LV_SYMBOL_VOLUME_MID, LV_SYMBOL_VOLUME_MAX, ""
};

// Physical key → button matrix index mapping (B=prev, P=play, N=next, M=mute, D=vol-, U=vol+)
static const struct { uint32_t key; uint32_t btnIdx; } KEY_MAP[] = {
    { 'b', 0 }, { 'B', 0 },
    { 'p', 1 }, { 'P', 1 },
    { 'n', 2 }, { 'N', 2 },
    { 'm', 3 }, { 'M', 3 },
    { 'd', 4 }, { 'D', 4 },
    { 'u', 5 }, { 'U', 5 },
};

// HID Consumer Page (0x0C) usage codes for each button, in BTN_MAP order
static const uint16_t CONSUMER_USAGE[6] = {
    0x00B6, // 0 – PREV:      Previous Track
    0x00CD, // 1 – PLAY:      Play/Pause
    0x00B5, // 2 – NEXT:      Next Track
    0x00E2, // 3 – MUTE:      Mute
    0x00EA, // 4 – VOL_DOWN:  Volume Decrement
    0x00E9, // 5 – VOL_UP:    Volume Increment
};

// Data passed to the one-shot key-send task
struct SendKeyData {
    struct Device* hidDevice;
    uint16_t usage;
};

// ---- Static task / callback implementations ----

void MediaKeys::sendKeyTask(void* param) {
    SendKeyData* data = static_cast<SendKeyData*>(param);

    uint8_t press[2] = {
        static_cast<uint8_t>(data->usage & 0xFF),
        static_cast<uint8_t>((data->usage >> 8) & 0xFF)
    };
    bluetooth_hid_device_send_consumer(data->hidDevice, press, 2);

    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t release[2] = {0, 0};
    bluetooth_hid_device_send_consumer(data->hidDevice, release, 2);

    delete data;
    vTaskDelete(nullptr);
}

void MediaKeys::onSwitchToggled(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    MediaKeys* self = static_cast<MediaKeys*>(lv_event_get_user_data(event));
    if (!self) return;
    bool enabled = lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED);
    self->handleSwitchToggle(enabled);
}

void MediaKeys::onButtonPressed(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    MediaKeys* self = static_cast<MediaKeys*>(lv_event_get_user_data(event));
    if (!self) return;
    uint32_t id = lv_btnmatrix_get_selected_btn(lv_event_get_target_obj(event));
    if (id != LV_BTNMATRIX_BTN_NONE) {
        self->handleButtonPress(id);
    }
}

void MediaKeys::onKeyEvent(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_KEY) return;
    MediaKeys* self = static_cast<MediaKeys*>(lv_event_get_user_data(event));
    if (!self || !self->_buttonMatrix) return;

    uint32_t key = lv_event_get_key(event);

    // Q or Esc exits key mode and returns focus to normal UI navigation
    if (key == 'q' || key == 'Q' || key == LV_KEY_ESC) {
        self->exitKeyMode();
        return;
    }

    if (!self->_isEnabled) return;

    for (auto& mapping : KEY_MAP) {
        if (mapping.key == key) {
            // Highlight: select the button and mark checked for visual feedback
            self->_activeKeyBtn = mapping.btnIdx;
            lv_buttonmatrix_set_selected_button(self->_buttonMatrix, mapping.btnIdx);
            lv_buttonmatrix_set_button_ctrl(self->_buttonMatrix, mapping.btnIdx, LV_BTNMATRIX_CTRL_CHECKED);

            // Restart the highlight-clear timer
            if (self->_keyHighlightTimer) {
                lv_timer_reset(self->_keyHighlightTimer);
                lv_timer_resume(self->_keyHighlightTimer);
            }

            self->handleButtonPress(mapping.btnIdx);
            return;
        }
    }
}

void MediaKeys::enterKeyMode() {
    if (_keyboardActive || !_buttonMatrix) return;
    lv_group_t* group = lv_group_get_default();
    if (!group) return;
    lv_group_add_obj(group, _buttonMatrix);
    lv_group_focus_obj(_buttonMatrix);
    lv_group_set_editing(group, true);
    _keyboardActive = true;
    LOG_I(TAG, "Key mode: ON (Q/Esc to exit)");
}

void MediaKeys::exitKeyMode() {
    if (!_keyboardActive || !_buttonMatrix) return;
    lv_group_t* group = lv_group_get_default();
    if (group) lv_group_set_editing(group, false);
    lv_group_remove_obj(_buttonMatrix);
    _keyboardActive = false;
    LOG_I(TAG, "Key mode: OFF");
}

void MediaKeys::onKeyHighlightTimer(lv_timer_t* t) {
    MediaKeys* self = static_cast<MediaKeys*>(lv_timer_get_user_data(t));
    if (!self || !self->_buttonMatrix) return;
    if (self->_activeKeyBtn != LV_BTNMATRIX_BTN_NONE) {
        lv_buttonmatrix_clear_button_ctrl(self->_buttonMatrix, self->_activeKeyBtn, LV_BTNMATRIX_CTRL_CHECKED);
        self->_activeKeyBtn = LV_BTNMATRIX_BTN_NONE;
    }
    lv_obj_remove_state(self->_buttonMatrix, LV_STATE_FOCUSED);
    lv_timer_pause(t);
}

void MediaKeys::btEventCallback(struct Device* /*device*/, void* context, struct BtEvent event) {
    MediaKeys* self = static_cast<MediaKeys*>(context);
    if (!self) return;

    if (event.type == BT_EVENT_RADIO_STATE_CHANGED) {
        LOG_I(TAG, "BT radio state: %d", (int)event.radio_state);

        if (event.radio_state == BT_RADIO_STATE_ON) {
            // Radio is now up - start HID (needs LVGL lock for UI update)
            if (tt_lvgl_lock(1000)) {
                // Re-check inside lock to avoid TOCTOU race with handleSwitchToggle(false)
                if (self->_radioEnabling) {
                    self->startHid();
                }
                tt_lvgl_unlock();
            }
        } else if (event.radio_state == BT_RADIO_STATE_OFF && self->_isEnabled) {
            // Radio dropped while we were active - revert UI
            LOG_I(TAG, "BT radio turned off, disabling HID");
            if (tt_lvgl_lock(1000)) {
                if (tt_lvgl_hardware_keyboard_is_available()) self->exitKeyMode();
                self->_hidDevice = nullptr;
                self->_isEnabled = false;
                self->_radioEnabling = false;
                if (self->_switchWidget) lv_obj_remove_state(self->_switchWidget, LV_STATE_CHECKED);
                if (self->_mainWrapper) lv_obj_add_flag(self->_mainWrapper, LV_OBJ_FLAG_HIDDEN);
                tt_lvgl_unlock();
            }
        }
    } else if (event.type == BT_EVENT_PROFILE_STATE_CHANGED && event.profile_state.profile == BT_PROFILE_HID_DEVICE) {
        LOG_I(TAG, "HID device: %s", event.profile_state.state == BT_PROFILE_STATE_CONNECTED ? "connected" : "disconnected");
    }
}

// ---- Instance methods ----

void MediaKeys::startHid() {
    // Called once the BT radio is confirmed ON (either already was, or just came up).
    // May be called from the BT event callback thread - LVGL must already be locked by caller.
    _radioEnabling = false;

    _hidDevice = bluetooth_hid_device_get_device();
    if (!_hidDevice) {
        LOG_E(TAG, "BLE HID device unavailable after radio on");
        if (_btDevice) bluetooth_remove_event_callback(_btDevice, btEventCallback);
        _btDevice = nullptr;
        _isEnabled = false;
        if (_switchWidget) lv_obj_remove_state(_switchWidget, LV_STATE_CHECKED);
        return;
    }

    error_t err = bluetooth_hid_device_start(_hidDevice, BT_HID_DEVICE_MODE_KEYBOARD);
    if (err != ERROR_NONE) {
        LOG_E(TAG, "Failed to start HID device: %d", (int)err);
        if (_btDevice) bluetooth_remove_event_callback(_btDevice, btEventCallback);
        _btDevice = nullptr;
        _hidDevice = nullptr;
        _isEnabled = false;
        if (_switchWidget) lv_obj_remove_state(_switchWidget, LV_STATE_CHECKED);
        return;
    }

    if (_mainWrapper) lv_obj_remove_flag(_mainWrapper, LV_OBJ_FLAG_HIDDEN);
    if (tt_lvgl_hardware_keyboard_is_available()) enterKeyMode();
}

void MediaKeys::handleSwitchToggle(bool enabled) {
    LOG_I(TAG, "Switch: %s", enabled ? "ON" : "OFF");
    _isEnabled = enabled;

    if (enabled) {
        _btDevice = bluetooth_find_first_ready_device();
        if (!_btDevice) {
            LOG_E(TAG, "No Bluetooth device found");
            _isEnabled = false;
            if (_switchWidget) lv_obj_remove_state(_switchWidget, LV_STATE_CHECKED);
            return;
        }

        bluetooth_set_device_name(_btDevice, "Tactility Media Keys");

        // Register callback before enabling radio so we don't miss the state-change event.
        bluetooth_add_event_callback(_btDevice, this, btEventCallback);

        enum BtRadioState radioState;
        bluetooth_get_radio_state(_btDevice, &radioState);

        if (radioState == BT_RADIO_STATE_ON) {
            // Radio already up - start HID immediately.
            _radioWasOff = false;
            startHid();
        } else {
            // Turn the radio on; startHid() will be called from btEventCallback
            // once BT_RADIO_STATE_ON fires.
            LOG_I(TAG, "BT radio not on (state=%d), enabling...", (int)radioState);
            _radioWasOff = true;
            _radioEnabling = true;
            bluetooth_set_radio_enabled(_btDevice, true);
        }
    } else {
        _radioEnabling = false;
        if (tt_lvgl_hardware_keyboard_is_available()) exitKeyMode();
        if (_hidDevice) bluetooth_hid_device_stop(_hidDevice);
        if (_btDevice) bluetooth_remove_event_callback(_btDevice, btEventCallback);
        if (_btDevice && _radioWasOff) bluetooth_set_radio_enabled(_btDevice, false);
        _radioWasOff = false;
        _btDevice = nullptr;
        _hidDevice = nullptr;
        if (_mainWrapper) lv_obj_add_flag(_mainWrapper, LV_OBJ_FLAG_HIDDEN);
    }
}

void MediaKeys::handleButtonPress(uint32_t buttonId) {
    if (!_hidDevice || !_isEnabled || buttonId >= 6) return;

    if (!bluetooth_hid_device_is_connected(_hidDevice)) {
        LOG_W(TAG, "Not connected, ignoring button %lu", buttonId);
        return;
    }

    LOG_I(TAG, "Button %lu pressed", buttonId);

    SendKeyData* data = new SendKeyData {_hidDevice, CONSUMER_USAGE[buttonId]};
    if (xTaskCreate(sendKeyTask, "bt_key", 4096, data, tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
        LOG_E(TAG, "Failed to create send task");
        delete data;
    }
}

void MediaKeys::onShow(AppHandle appHandle, lv_obj_t* parent) {
    _parent = parent;
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* toolbar = tt_lvgl_toolbar_create_for_app(parent, appHandle);
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 0);

    _switchWidget = tt_lvgl_toolbar_add_switch_action(toolbar);
    lv_obj_add_event_cb(_switchWidget, onSwitchToggled, LV_EVENT_VALUE_CHANGED, this);

    _mainWrapper = lv_obj_create(parent);
    lv_obj_set_style_bg_color(_mainWrapper, lv_palette_darken(LV_PALETTE_GREY, 4), LV_PART_MAIN);
    lv_obj_set_width(_mainWrapper, LV_PCT(100));
    lv_obj_set_flex_grow(_mainWrapper, 1);
    lv_obj_set_style_pad_all(_mainWrapper, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(_mainWrapper, 0, LV_PART_MAIN);
    lv_obj_remove_flag(_mainWrapper, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(_mainWrapper, LV_FLEX_FLOW_COLUMN);

    _buttonMatrix = lv_btnmatrix_create(_mainWrapper);
    lv_btnmatrix_set_map(_buttonMatrix, BTN_MAP);
    lv_obj_set_style_text_font(_buttonMatrix, lvgl_get_text_font(FONT_SIZE_LARGE), LV_PART_ITEMS);
    lv_obj_set_style_pad_all(_buttonMatrix, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_row(_buttonMatrix, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_column(_buttonMatrix, 5, LV_PART_MAIN);
    lv_obj_set_style_border_width(_buttonMatrix, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_buttonMatrix, 0, LV_PART_MAIN);

    if (lv_display_get_horizontal_resolution(nullptr) <= 240 ||
        lv_display_get_vertical_resolution(nullptr) <= 240) {
        lv_obj_set_size(_buttonMatrix, lv_pct(100), lv_pct(70));
    } else {
        lv_obj_set_size(_buttonMatrix, lv_pct(100), lv_pct(85));
    }
    lv_obj_set_flex_grow(_buttonMatrix, 1);

    lv_obj_add_event_cb(_buttonMatrix, onButtonPressed, LV_EVENT_VALUE_CHANGED, this);

    // Physical keyboard support: key events on the matrix (entered when BT enabled, Q/Esc exits)
    if (tt_lvgl_hardware_keyboard_is_available()) {
        lv_obj_add_event_cb(_buttonMatrix, onKeyEvent, LV_EVENT_KEY, this);
        _keyHighlightTimer = lv_timer_create(onKeyHighlightTimer, 150, this);
        lv_timer_pause(_keyHighlightTimer);
    }

    lv_obj_add_flag(_mainWrapper, LV_OBJ_FLAG_HIDDEN);
}

void MediaKeys::onHide(AppHandle /*appHandle*/) {
    if (_hidDevice) bluetooth_hid_device_stop(_hidDevice);
    if (_btDevice) bluetooth_remove_event_callback(_btDevice, btEventCallback);
    if (_btDevice && _radioWasOff) bluetooth_set_radio_enabled(_btDevice, false);
    _btDevice = nullptr;
    _hidDevice = nullptr;
    _isEnabled = false;
    _radioEnabling = false;
    _radioWasOff = false;

    if (tt_lvgl_hardware_keyboard_is_available()) exitKeyMode();
    if (_keyHighlightTimer) {
        lv_timer_delete(_keyHighlightTimer);
        _keyHighlightTimer = nullptr;
    }
    _activeKeyBtn = LV_BTNMATRIX_BTN_NONE;

    _parent = nullptr;
    _mainWrapper = nullptr;
    _switchWidget = nullptr;
    _buttonMatrix = nullptr;
}
