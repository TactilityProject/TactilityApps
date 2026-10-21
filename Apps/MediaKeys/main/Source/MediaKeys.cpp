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
const char* DEVICE_NAME = "Tactility Media Keys";

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

constexpr uint32_t WORKER_STACK_SIZE = 6144;
constexpr UBaseType_t WORKER_QUEUE_LENGTH = 8;

enum class WorkerCommandType : uint8_t {
    EnableRadio,
    StartHid,
    StartHidAfterRadioOn,
    Disable,
    Shutdown,
};

struct WorkerCommand {
    WorkerCommandType type;
};

// Data passed to the one-shot key-send task
struct SendKeyData {
    struct Device* hidDevice;
    uint16_t usage;
};

void enterKeyMode(Context* ctx);
void exitKeyMode(Context* ctx);
void startHid(Context* ctx, bool renameAfterStart);
void postWorkerCommand(Context* ctx, WorkerCommandType type);
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

    device_put(data->hidDevice);
    delete data;
    vTaskDelete(nullptr);
}

void onSwitchToggled(lv_event_t* event);
void onButtonPressed(lv_event_t* event);
void onKeyEvent(lv_event_t* event);
void onKeyHighlightTimer(lv_timer_t* t);
void handleBtEvent(Context* ctx, const BtEvent& event);
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

void handleBtEvent(Context* ctx, const BtEvent& event) {
    if (event.type == BT_EVENT_RADIO_STATE_CHANGED) {
        LOG_I(TAG, "BT radio state: %d", (int)event.radio_state);

        if (event.radio_state == BT_RADIO_STATE_ON) {
            // Radio is now up. The worker starts HID only if the user didn't switch off in the meantime.
            if (ctx->radioEnabling) {
                postWorkerCommand(ctx, WorkerCommandType::StartHidAfterRadioOn);
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
                if (ctx->hidDevice) device_put(ctx->hidDevice);
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

void postWorkerCommand(Context* ctx, WorkerCommandType type) {
    if (ctx->workerQueue == nullptr) return;
    WorkerCommand command { type };
    if (xQueueSend(ctx->workerQueue, &command, 0) != pdTRUE) {
        LOG_E(TAG, "Worker queue full");
    }
}

// Takes the LVGL lock from the worker. @return false when the lock couldn't be taken
bool lockLvgl() {
    for (int i = 0; i < 5; i++) {
        if (lvgl_try_lock(1000)) return true;
    }
    LOG_W(TAG, "Failed to acquire LVGL lock");
    return false;
}

// Runs on the worker. Reverts the switch when HID can't start.
void showHidFailed(Context* ctx) {
    ctx->isEnabled = false;
    if (!lockLvgl()) return;
    if (window_manager_get_state(ctx->window) == WINDOW_STATE_GRANTED && ctx->switchWidget) {
        lv_obj_remove_state(ctx->switchWidget, LV_STATE_CHECKED);
    }
    lvgl_unlock();
}

// Runs on the worker, once the BT radio is confirmed ON (either already was, or just came up).
void startHid(Context* ctx, bool renameAfterStart) {
    ctx->radioEnabling = false;

    Device* hidDevice = bluetooth_hid_device_get();
    if (!hidDevice) {
        LOG_E(TAG, "BLE HID device unavailable after radio on");
        showHidFailed(ctx);
        return;
    }

    error_t err = bluetooth_hid_device_start(hidDevice, BT_HID_DEVICE_MODE_KEYBOARD);
    if (err != ERROR_NONE) {
        LOG_E(TAG, "Failed to start HID device: %d", (int)err);
        device_put(hidDevice);
        showHidFailed(ctx);
        return;
    }
    ctx->hidDevice = hidDevice;

    // Rename only after HID is up: with the radio on, a rename restarts advertising
    // right away, and the GATT profile switch in bluetooth_hid_device_start() asserts in
    // NimBLE (ble_svc_gap_init) if it runs while that advertising is still active.
    // Once HID is active, the rename just restarts HID advertising with the new name.
    if (renameAfterStart) bluetooth_set_device_name(ctx->btDevice, DEVICE_NAME);

    // Skip touching widgets if the window was buried in the meantime (window_manager may have
    // already deleted them), same reasoning as onKeyHighlightTimer above.
    // Also skip if the switch was turned off while HID was starting: handleSwitchToggle already
    // hid the controls and queued Disable, which stops HID.
    if (!lockLvgl()) return;
    if (ctx->isEnabled && window_manager_get_state(ctx->window) == WINDOW_STATE_GRANTED) {
        if (ctx->mainWrapper) lv_obj_remove_flag(ctx->mainWrapper, LV_OBJ_FLAG_HIDDEN);
        if (device_has_active_by_type(&KEYBOARD_TYPE)) enterKeyMode(ctx);
    }
    lvgl_unlock();
}

// Restores the radio to the state we found it in when we turned it on ourselves. Does not touch
// the BT event subscription - that's app-lifetime, set up by mediaKeysInitBt()/torn down by
// teardownBt() at app exit, not per toggle.
void restoreRadioIfNeeded(Context* ctx) {
    if (ctx->btDevice && ctx->radioWasOff) bluetooth_set_radio_enabled(ctx->btDevice, false);
    ctx->radioWasOff = false;
}

void workerTask(void* param) {
    auto* ctx = static_cast<Context*>(param);
    WorkerCommand command;
    bool running = true;
    while (running) {
        xQueueReceive(ctx->workerQueue, &command, portMAX_DELAY);
        switch (command.type) {
            case WorkerCommandType::EnableRadio:
                bluetooth_set_device_name(ctx->btDevice, DEVICE_NAME);
                bluetooth_set_radio_enabled(ctx->btDevice, true);
                break;
            case WorkerCommandType::StartHid:
                // Re-check here to avoid a race with a switch-off that happened after the command was queued
                if (ctx->isEnabled) startHid(ctx, true);
                break;
            case WorkerCommandType::StartHidAfterRadioOn:
                // Re-check here to avoid a race with a switch-off that happened after the event was queued
                if (ctx->isEnabled && ctx->radioEnabling) startHid(ctx, false);
                break;
            case WorkerCommandType::Disable:
                if (ctx->hidDevice) {
                    bluetooth_hid_device_stop(ctx->hidDevice);
                    device_put(ctx->hidDevice);
                    ctx->hidDevice = nullptr;
                }
                restoreRadioIfNeeded(ctx);
                break;
            case WorkerCommandType::Shutdown:
                restoreRadioIfNeeded(ctx);
                running = false;
                break;
        }
    }
    xSemaphoreGive(ctx->workerDone);
    vTaskDelete(nullptr);
}

void teardownBt(Context* ctx) {
    // Unsubscribe FIRST - stops any in-flight BT events from firing against our (possibly
    // already freed) UI widget pointers after this returns.
    if (ctx->btDevice) bluetooth_event_unsubscribe(ctx->btDevice, &ctx->btEventSub);
    // Do NOT call bluetooth_hid_device_stop here: it calls ble_gatts_reset() /
    // ble_gatts_start() which corrupts NimBLE heap while the host task is still
    // running. HID device is a persistent kernel device; hid_device_start() cleans
    // up stale context on next use. Explicit stop is handled by handleSwitchToggle.
    // The device itself is never stopped, it's started for the process lifetime (see
    // Documentation/bluetooth-app-migration.md) - just restore the radio and drop our ref.
    if (ctx->workerQueue != nullptr) {
        // Blocking send: dropping Shutdown on a full queue would hang the workerDone wait below.
        // The worker keeps draining the queue, so this completes.
        WorkerCommand shutdown { WorkerCommandType::Shutdown };
        xQueueSend(ctx->workerQueue, &shutdown, portMAX_DELAY);
        xSemaphoreTake(ctx->workerDone, portMAX_DELAY);
        vSemaphoreDelete(ctx->workerDone);
        vQueueDelete(ctx->workerQueue);
        ctx->workerDone = nullptr;
        ctx->workerQueue = nullptr;
    }
    if (ctx->btDevice) device_put(ctx->btDevice);
    if (ctx->hidDevice) device_put(ctx->hidDevice);
    ctx->btDevice = nullptr;
    ctx->hidDevice = nullptr;
}

void handleSwitchToggle(Context* ctx, bool enabled) {
    LOG_I(TAG, "Switch: %s", enabled ? "ON" : "OFF");
    ctx->isEnabled = enabled;

    if (enabled) {
        if (!ctx->btDevice) {
            LOG_E(TAG, "No Bluetooth device found");
            ctx->isEnabled = false;
            if (ctx->switchWidget) lv_obj_remove_state(ctx->switchWidget, LV_STATE_CHECKED);
            return;
        }

        enum BtRadioState radioState;
        bluetooth_get_radio_state(ctx->btDevice, &radioState);

        // The Bluetooth calls run on the worker: this can be the LVGL task, which must not touch flash
        if (radioState == BT_RADIO_STATE_ON) {
            // Radio already up - start HID immediately.
            ctx->radioWasOff = false;
            postWorkerCommand(ctx, WorkerCommandType::StartHid);
        } else {
            // Turn the radio on; the worker starts HID from handleBtEvent()
            // once BT_RADIO_STATE_ON fires. Our BT event subscription (mediaKeysInitBt())
            // was already claimed before the app's main loop started waiting, so that
            // wakeup won't be missed.
            LOG_I(TAG, "BT radio not on (state=%d), enabling...", (int)radioState);
            ctx->radioWasOff = true;
            ctx->radioEnabling = true;
            postWorkerCommand(ctx, WorkerCommandType::EnableRadio);
        }
    } else {
        ctx->radioEnabling = false;
        if (device_has_active_by_type(&KEYBOARD_TYPE)) exitKeyMode(ctx);
        // Explicit user toggle-off: the worker stops HID cleanly and restores the radio
        // (safe here since the user intentionally disabled, so no race with app teardown).
        postWorkerCommand(ctx, WorkerCommandType::Disable);
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

    // The task runs across a 50ms delay outside the caller's stack frame - it needs its own
    // reference so teardownBt()'s device_put() can't invalidate ctx->hidDevice out from under it.
    if (device_get(ctx->hidDevice) != ERROR_NONE) {
        LOG_E(TAG, "Failed to acquire HID device reference for send task");
        return;
    }

    SendKeyData* data = new SendKeyData {ctx->hidDevice, CONSUMER_USAGE[buttonId]};
    if (xTaskCreate(sendKeyTask, "bt_key", 4096, data, tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
        LOG_E(TAG, "Failed to create send task");
        device_put(data->hidDevice);
        delete data;
    }
}

} // namespace

bool mediaKeysInitBt(Context* ctx) {
    // ble0 is started for the process lifetime once enabled in the devicetree - just look it up,
    // no device_start() needed (see Documentation/bluetooth-app-migration.md).
    if (device_get_first_by_type(&BLUETOOTH_TYPE, &ctx->btDevice) != ERROR_NONE) {
        LOG_E(TAG, "No Bluetooth device found");
        ctx->btDevice = nullptr;
        return false;
    }
    // Must claim this subscription's bit before the app's main loop makes its first
    // task_event_group_wait_any() call - a bit claimed mid-wait isn't included until the next
    // call, so a radio-on event fired right after a late subscribe could go unnoticed forever.
    if (bluetooth_event_subscribe(ctx->btDevice, &ctx->btEventSub, ctx->eventGroup) != ERROR_NONE) {
        LOG_E(TAG, "Failed to subscribe to BT events");
        device_put(ctx->btDevice);
        ctx->btDevice = nullptr;
        return false;
    }
    ctx->workerQueue = xQueueCreate(WORKER_QUEUE_LENGTH, sizeof(WorkerCommand));
    ctx->workerDone = xSemaphoreCreateBinary();
    if (ctx->workerQueue == nullptr || ctx->workerDone == nullptr ||
        xTaskCreate(workerTask, "mediakeys_bt", WORKER_STACK_SIZE, ctx, tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
        LOG_E(TAG, "Failed to start Bluetooth worker");
        if (ctx->workerQueue) vQueueDelete(ctx->workerQueue);
        if (ctx->workerDone) vSemaphoreDelete(ctx->workerDone);
        ctx->workerQueue = nullptr;
        ctx->workerDone = nullptr;
        bluetooth_event_unsubscribe(ctx->btDevice, &ctx->btEventSub);
        device_put(ctx->btDevice);
        ctx->btDevice = nullptr;
        return false;
    }
    return true;
}

void mediaKeysProcessBtEvents(Context* ctx) {
    if (ctx->btDevice == nullptr) return;
    BtEvent event {};
    while (bluetooth_event_poll(&ctx->btEventSub, &event) == ERROR_NONE) {
        handleBtEvent(ctx, event);
    }
}

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
    // ctx->btDevice was already acquired by mediaKeysInitBt() before this ran.
    if (ctx->btDevice && device_is_ready(ctx->btDevice)) {
        enum BtRadioState radioState;
        if (bluetooth_get_radio_state(ctx->btDevice, &radioState) == ERROR_NONE && radioState == BT_RADIO_STATE_ON) {
            lv_obj_add_state(ctx->switchWidget, LV_STATE_CHECKED);
            handleSwitchToggle(ctx, true);
        }
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
