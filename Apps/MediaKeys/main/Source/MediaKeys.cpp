#include "MediaKeys.h"

#include <tactility/log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl/fonts.h>
#include <lvgl/lvgl.h>
#include <lvgl/widgets/toolbar.h>

namespace {

const char* TAG = "MediaKeys";

// Button layout: two rows of three media-control buttons
const char* BTN_MAP[] = {
    LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT, "\n",
    LV_SYMBOL_MUTE, LV_SYMBOL_VOLUME_MID, LV_SYMBOL_VOLUME_MAX, ""
};

// Physical key → button matrix index mapping (B=prev, P=play, N=next, M=mute, D=vol-, U=vol+)
const struct { uint32_t key; uint32_t btnIdx; } KEY_MAP[] = {
    { 'b', 0 }, { 'B', 0 },
    { 'p', 1 }, { 'P', 1 },
    { 'n', 2 }, { 'N', 2 },
    { 'm', 3 }, { 'M', 3 },
    { 'd', 4 }, { 'D', 4 },
    { 'u', 5 }, { 'U', 5 },
};

// HID Consumer Page (0x0C) usage codes for each button, in BTN_MAP order
const uint16_t CONSUMER_USAGE[6] = {
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

void enterKeyMode(Context* ctx);
void exitKeyMode(Context* ctx);
void startHid(Context* ctx);
void handleButtonPress(Context* ctx, uint32_t buttonId);

// ---- Static task / callback implementations ----

void sendKeyTask(void* param) {
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

void onSwitchToggled(lv_event_t* event);
void onButtonPressed(lv_event_t* event);
void onKeyEvent(lv_event_t* event);
void onKeyHighlightTimer(lv_timer_t* t);
void btEventCallback(struct Device* device, void* context, struct BtEvent event);
void handleSwitchToggle(Context* ctx, bool enabled);

void onSwitchToggled(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    if (!ctx) return;
    bool enabled = lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED);
    handleSwitchToggle(ctx, enabled);
}

void onButtonPressed(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    if (!ctx) return;
    uint32_t id = lv_btnmatrix_get_selected_btn(lv_event_get_target_obj(event));
    if (id != LV_BTNMATRIX_BTN_NONE) {
        handleButtonPress(ctx, id);
    }
}

void onKeyEvent(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_KEY) return;
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    if (!ctx || !ctx->buttonMatrix) return;

    uint32_t key = lv_event_get_key(event);

    // Q or Esc exits key mode and returns focus to normal UI navigation
    if (key == 'q' || key == 'Q' || key == LV_KEY_ESC) {
        exitKeyMode(ctx);
        return;
    }

    if (!ctx->isEnabled) return;

    for (auto& mapping : KEY_MAP) {
        if (mapping.key == key) {
            // Highlight: select the button and mark checked for visual feedback
            ctx->activeKeyBtn = mapping.btnIdx;
            lv_buttonmatrix_set_selected_button(ctx->buttonMatrix, mapping.btnIdx);
            lv_buttonmatrix_set_button_ctrl(ctx->buttonMatrix, mapping.btnIdx, LV_BTNMATRIX_CTRL_CHECKED);

            // Restart the highlight-clear timer
            if (ctx->keyHighlightTimer) {
                lv_timer_reset(ctx->keyHighlightTimer);
                lv_timer_resume(ctx->keyHighlightTimer);
            }

            handleButtonPress(ctx, mapping.btnIdx);
            return;
        }
    }
}

void enterKeyMode(Context* ctx) {
    if (ctx->keyboardActive || !ctx->buttonMatrix) return;
    lv_group_t* group = lv_group_get_default();
    if (!group) return;
    lv_group_add_obj(group, ctx->buttonMatrix);
    lv_group_focus_obj(ctx->buttonMatrix);
    lv_group_set_editing(group, true);
    ctx->keyboardActive = true;
    LOG_I(TAG, "Key mode: ON (Q/Esc to exit)");
}

void exitKeyMode(Context* ctx) {
    if (!ctx->keyboardActive || !ctx->buttonMatrix) return;
    lv_group_t* group = lv_group_get_default();
    if (group) lv_group_set_editing(group, false);
    lv_group_remove_obj(ctx->buttonMatrix);
    ctx->keyboardActive = false;
    LOG_I(TAG, "Key mode: OFF");
}

void onKeyHighlightTimer(lv_timer_t* t) {
    auto* ctx = static_cast<Context*>(lv_timer_get_user_data(t));
    if (!ctx || !ctx->buttonMatrix) return;
    // Widgets only exist while this window is topmost - skip otherwise (same reasoning as
    // GPIO.cpp's periodic status timer: window_manager deletes a buried window's widgets, so
    // touching buttonMatrix here would use-after-free it). The timer itself isn't destroyed by
    // burial, only by mediaKeysTeardown(), so it can still fire while buried.
    if (window_manager_get_state(ctx->window) != WINDOW_STATE_GRANTED) return;
    if (ctx->activeKeyBtn != LV_BTNMATRIX_BTN_NONE) {
        lv_buttonmatrix_clear_button_ctrl(ctx->buttonMatrix, ctx->activeKeyBtn, LV_BTNMATRIX_CTRL_CHECKED);
        ctx->activeKeyBtn = LV_BTNMATRIX_BTN_NONE;
    }
    lv_obj_remove_state(ctx->buttonMatrix, LV_STATE_FOCUSED);
    lv_timer_pause(t);
}

void btEventCallback(struct Device* /*device*/, void* context, struct BtEvent event) {
    auto* ctx = static_cast<Context*>(context);
    if (!ctx) return;

    if (event.type == BT_EVENT_RADIO_STATE_CHANGED) {
        LOG_I(TAG, "BT radio state: %d", (int)event.radio_state);

        if (event.radio_state == BT_RADIO_STATE_ON) {
            // Radio is now up - start HID (needs LVGL lock for UI update)
            if (lvgl_try_lock(1000)) {
                // Re-check inside lock to avoid TOCTOU race with handleSwitchToggle(false).
                // Also skip touching widgets if this window has been buried in the meantime
                // (window_manager may have already deleted them) - same reasoning as
                // onKeyHighlightTimer above.
                if (ctx->radioEnabling && window_manager_get_state(ctx->window) == WINDOW_STATE_GRANTED) {
                    startHid(ctx);
                }
                lvgl_unlock();
            }
        } else if (event.radio_state == BT_RADIO_STATE_OFF && ctx->isEnabled) {
            // Radio dropped while we were active - revert UI
            LOG_I(TAG, "BT radio turned off, disabling HID");
            if (lvgl_try_lock(1000)) {
                if (window_manager_get_state(ctx->window) == WINDOW_STATE_GRANTED) {
                    if (device_has_active_by_type(&KEYBOARD_TYPE)) exitKeyMode(ctx);
                    if (ctx->switchWidget) lv_obj_remove_state(ctx->switchWidget, LV_STATE_CHECKED);
                    if (ctx->mainWrapper) lv_obj_add_flag(ctx->mainWrapper, LV_OBJ_FLAG_HIDDEN);
                }
                ctx->hidDevice = nullptr;
                ctx->isEnabled = false;
                ctx->radioEnabling = false;
                lvgl_unlock();
            }
        }
    } else if (event.type == BT_EVENT_PROFILE_STATE_CHANGED && event.profile_state.profile == BT_PROFILE_HID_DEVICE) {
        LOG_I(TAG, "HID device: %s", event.profile_state.state == BT_PROFILE_STATE_CONNECTED ? "connected" : "disconnected");
    }
}

// ---- Instance-equivalent functions ----

void startHid(Context* ctx) {
    // Called once the BT radio is confirmed ON (either already was, or just came up).
    // May be called from the BT event callback thread - LVGL must already be locked by caller.
    ctx->radioEnabling = false;

    ctx->hidDevice = bluetooth_hid_device_get_device();
    if (!ctx->hidDevice) {
        LOG_E(TAG, "BLE HID device unavailable after radio on");
        if (ctx->btDevice) {
            bluetooth_remove_event_callback(ctx->btDevice, btEventCallback);
            device_put(ctx->btDevice);
            ctx->btDevice = nullptr;
        }
        ctx->isEnabled = false;
        if (ctx->switchWidget) lv_obj_remove_state(ctx->switchWidget, LV_STATE_CHECKED);
        return;
    }

    error_t err = bluetooth_hid_device_start(ctx->hidDevice, BT_HID_DEVICE_MODE_KEYBOARD);
    if (err != ERROR_NONE) {
        LOG_E(TAG, "Failed to start HID device: %d", (int)err);
        if (ctx->btDevice) {
            bluetooth_remove_event_callback(ctx->btDevice, btEventCallback);
            device_put(ctx->btDevice);
            ctx->btDevice = nullptr;
        }
        ctx->hidDevice = nullptr;
        ctx->isEnabled = false;
        if (ctx->switchWidget) lv_obj_remove_state(ctx->switchWidget, LV_STATE_CHECKED);
        return;
    }

    if (ctx->mainWrapper) lv_obj_remove_flag(ctx->mainWrapper, LV_OBJ_FLAG_HIDDEN);
    if (device_has_active_by_type(&KEYBOARD_TYPE)) enterKeyMode(ctx);
}

void teardownBt(Context* ctx) {
    // Remove callback FIRST - stops any in-flight BT events from firing against
    // our (possibly already freed) UI widget pointers after this returns.
    if (ctx->btDevice) bluetooth_remove_event_callback(ctx->btDevice, btEventCallback);
    // Do NOT call bluetooth_hid_device_stop here: it calls ble_gatts_reset() /
    // ble_gatts_start() which corrupts NimBLE heap while the host task is still
    // running. HID device is a persistent kernel device; hid_device_start() cleans
    // up stale context on next use. Explicit stop is handled by handleSwitchToggle.
    // Restore the radio/device to the state we found them in.
    if (ctx->btDevice && ctx->radioWasOff) bluetooth_set_radio_enabled(ctx->btDevice, false);
    if (ctx->btDevice && ctx->deviceWasStarted) device_stop(ctx->btDevice);
    if (ctx->btDevice) device_put(ctx->btDevice);
    ctx->btDevice = nullptr;
    ctx->hidDevice = nullptr;
    ctx->radioWasOff = false;
    ctx->deviceWasStarted = false;
}

void handleSwitchToggle(Context* ctx, bool enabled) {
    LOG_I(TAG, "Switch: %s", enabled ? "ON" : "OFF");
    ctx->isEnabled = enabled;

    if (enabled) {
        if (device_get_first_by_type(&BLUETOOTH_TYPE, &ctx->btDevice) != ERROR_NONE) {
            LOG_E(TAG, "No Bluetooth device found");
            ctx->btDevice = nullptr;
            ctx->isEnabled = false;
            if (ctx->switchWidget) lv_obj_remove_state(ctx->switchWidget, LV_STATE_CHECKED);
            return;
        }

        // Device may not be started yet (BT disabled in DTS by default to save memory).
        if (!device_is_ready(ctx->btDevice)) {
            LOG_I(TAG, "BT device not started, starting now");
            if (device_start(ctx->btDevice) != ERROR_NONE) {
                LOG_E(TAG, "Failed to start BT device");
                device_put(ctx->btDevice);
                ctx->btDevice = nullptr;
                ctx->isEnabled = false;
                if (ctx->switchWidget) lv_obj_remove_state(ctx->switchWidget, LV_STATE_CHECKED);
                return;
            }
            ctx->deviceWasStarted = true;
        }

        bluetooth_set_device_name(ctx->btDevice, "Tactility Media Keys");

        // Register callback before enabling radio so we don't miss the state-change event.
        bluetooth_add_event_callback(ctx->btDevice, ctx, btEventCallback);

        enum BtRadioState radioState;
        bluetooth_get_radio_state(ctx->btDevice, &radioState);

        if (radioState == BT_RADIO_STATE_ON) {
            // Radio already up - start HID immediately.
            ctx->radioWasOff = false;
            startHid(ctx);
        } else {
            // Turn the radio on; startHid() will be called from btEventCallback
            // once BT_RADIO_STATE_ON fires.
            LOG_I(TAG, "BT radio not on (state=%d), enabling...", (int)radioState);
            ctx->radioWasOff = true;
            ctx->radioEnabling = true;
            bluetooth_set_radio_enabled(ctx->btDevice, true);
        }
    } else {
        ctx->radioEnabling = false;
        if (device_has_active_by_type(&KEYBOARD_TYPE)) exitKeyMode(ctx);
        // Explicit user toggle-off: stop HID cleanly (safe here since we're on the
        // LVGL task and the user intentionally disabled, so no race with app teardown).
        if (ctx->hidDevice) bluetooth_hid_device_stop(ctx->hidDevice);
        teardownBt(ctx);
        if (ctx->mainWrapper) lv_obj_add_flag(ctx->mainWrapper, LV_OBJ_FLAG_HIDDEN);
    }
}

void handleButtonPress(Context* ctx, uint32_t buttonId) {
    if (!ctx->hidDevice || !ctx->isEnabled || buttonId >= 6) return;

    if (!bluetooth_hid_device_is_connected(ctx->hidDevice)) {
        LOG_W(TAG, "Not connected, ignoring button %lu", buttonId);
        return;
    }

    LOG_I(TAG, "Button %lu pressed", buttonId);

    SendKeyData* data = new SendKeyData {ctx->hidDevice, CONSUMER_USAGE[buttonId]};
    if (xTaskCreate(sendKeyTask, "bt_key", 4096, data, tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
        LOG_E(TAG, "Failed to create send task");
        delete data;
    }
}

} // namespace

void mediaKeysCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* toolbar = lvgl_toolbar_create(parent, "Media Keys");
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 0);

    ctx->switchWidget = lvgl_toolbar_add_switch_action(toolbar);
    lv_obj_add_event_cb(ctx->switchWidget, onSwitchToggled, LV_EVENT_VALUE_CHANGED, ctx);

    ctx->mainWrapper = lv_obj_create(parent);
    lv_obj_set_style_bg_color(ctx->mainWrapper, lv_palette_darken(LV_PALETTE_GREY, 4), LV_PART_MAIN);
    lv_obj_set_width(ctx->mainWrapper, LV_PCT(100));
    lv_obj_set_flex_grow(ctx->mainWrapper, 1);
    lv_obj_set_style_pad_all(ctx->mainWrapper, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->mainWrapper, 0, LV_PART_MAIN);
    lv_obj_remove_flag(ctx->mainWrapper, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ctx->mainWrapper, LV_FLEX_FLOW_COLUMN);

    ctx->buttonMatrix = lv_btnmatrix_create(ctx->mainWrapper);
    lv_btnmatrix_set_map(ctx->buttonMatrix, BTN_MAP);
    lv_obj_set_style_text_font(ctx->buttonMatrix, lvgl_get_text_font(FONT_SIZE_LARGE), LV_PART_ITEMS);
    lv_obj_set_style_pad_all(ctx->buttonMatrix, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_row(ctx->buttonMatrix, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_column(ctx->buttonMatrix, 5, LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->buttonMatrix, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctx->buttonMatrix, 0, LV_PART_MAIN);

    if (lv_display_get_horizontal_resolution(nullptr) <= 240 ||
        lv_display_get_vertical_resolution(nullptr) <= 240) {
        lv_obj_set_size(ctx->buttonMatrix, lv_pct(100), lv_pct(70));
    } else {
        lv_obj_set_size(ctx->buttonMatrix, lv_pct(100), lv_pct(85));
    }
    lv_obj_set_flex_grow(ctx->buttonMatrix, 1);

    lv_obj_add_event_cb(ctx->buttonMatrix, onButtonPressed, LV_EVENT_VALUE_CHANGED, ctx);

    // Physical keyboard support: key events on the matrix (entered when BT enabled, Q/Esc exits)
    if (device_has_active_by_type(&KEYBOARD_TYPE)) {
        lv_obj_add_event_cb(ctx->buttonMatrix, onKeyEvent, LV_EVENT_KEY, ctx);
        ctx->keyHighlightTimer = lv_timer_create(onKeyHighlightTimer, 150, ctx);
        lv_timer_pause(ctx->keyHighlightTimer);
    }

    lv_obj_add_flag(ctx->mainWrapper, LV_OBJ_FLAG_HIDDEN);

    // Auto-enable if BT is already on (turned on via QuickPanel/Settings before opening app).
    // Transient lookup only - handleSwitchToggle() acquires its own reference into ctx->btDevice.
    struct Device* btDev = nullptr;
    if (device_get_first_by_type(&BLUETOOTH_TYPE, &btDev) == ERROR_NONE) {
        if (device_is_ready(btDev)) {
            enum BtRadioState radioState;
            if (bluetooth_get_radio_state(btDev, &radioState) == ERROR_NONE && radioState == BT_RADIO_STATE_ON) {
                lv_obj_add_state(ctx->switchWidget, LV_STATE_CHECKED);
                handleSwitchToggle(ctx, true);
            }
        }
        device_put(btDev);
    }
}

void mediaKeysTeardown(Context* ctx) {
    ctx->radioEnabling = false;
    ctx->isEnabled = false;
    // Don't call exitKeyMode() here: by the time this runs, window_manager_remove() has already
    // deleted buttonMatrix, and LVGL detaches a deleted object from its group automatically as
    // part of that deletion - calling lv_group_remove_obj() on it here would be a use-after-free
    // (same reasoning as Magic8Ball.cpp's teardown). Just clear editing mode on the default
    // group directly, since that's independent of buttonMatrix's lifetime.
    if (ctx->keyboardActive) {
        lv_group_t* group = lv_group_get_default();
        if (group) lv_group_set_editing(group, false);
        ctx->keyboardActive = false;
    }
    teardownBt(ctx);
    // keyHighlightTimer is an app-owned lv_timer_t, not part of the window's widget subtree, so
    // window_manager_remove() never touches it - still safe (and necessary) to delete here.
    if (ctx->keyHighlightTimer) {
        lv_timer_delete(ctx->keyHighlightTimer);
        ctx->keyHighlightTimer = nullptr;
    }
    ctx->activeKeyBtn = LV_BTNMATRIX_BTN_NONE;

    ctx->mainWrapper = nullptr;
    ctx->switchWidget = nullptr;
    ctx->buttonMatrix = nullptr;
}
