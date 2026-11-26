// SPDX-License-Identifier: GPL-3.0-only
#include <lvgl_window_manager/dialog.h>
#include <screensaver.h>

#include <app/event.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>
#include <tactility/log.h>
#include <tactility/module.h>

#include <lvgl.h>
#include <lvgl/lvgl.h>
#include <lvgl/widgets/card.h>
#include <lvgl/widgets/toolbar.h>

#include <iterator>
#include <type_traits>

namespace {

constexpr auto* TAG = "ScreensaverApp";

// Tables of pointers aren't relocated when the app is loaded, so the names are one string
constexpr char TIMEOUT_NAMES[] = "15 seconds\n30 seconds\n1 minute\n2 minutes\n5 minutes";
constexpr uint32_t TIMEOUT_MS[] = { 15000, 30000, 60000, 120000, 300000 };

// In ScreensaverType order
constexpr char TYPE_NAMES[] = "None\nBouncing Balls\nMystify\nMatrix Rain\nStackChan";

/**
 * The functions of the screensaver module. They are looked up while the app runs instead of linked,
 * so the app can start without the module and tell the user about it.
 */
struct ModuleApi {
    decltype(&screensaver_settings_load) loadSettings = nullptr;
    decltype(&screensaver_settings_save) saveSettings = nullptr;
    decltype(&screensaver_preview) preview = nullptr;
    // Holds a reference, so the module can't stop while the app uses its functions
    Module* module = nullptr;
};

struct Context {
    uint32_t appInstanceId = 0;
    ModuleApi api {};
    // Signalled when the dialog that tells the module isn't running closes
    uint32_t dialogBit = 0;
    TaskEventGroup* eventGroup = nullptr;
    uint32_t previewBit = 0;

    // Written by the app task and by LVGL callbacks, with the LVGL lock held
    ScreensaverSettings settings {};
    bool changed = false;

    // nullptr while the widgets don't exist
    lv_obj_t* enableSwitch = nullptr;
    lv_obj_t* optionsCard = nullptr;
    lv_obj_t* timeoutDropdown = nullptr;
    lv_obj_t* typeDropdown = nullptr;
};

/** @return false when the module isn't running. Otherwise release it with releaseModule(). */
bool acquireModule(ModuleApi& api) {
    Module* module = nullptr;
    auto resolve = [&module](const char* name, auto& function) {
        uintptr_t address = 0;
        Module* owner = nullptr;
        if (!module_resolve_symbol_global_get(name, &address, &owner)) {
            return false;
        }
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(address);
        // One reference per module is enough
        if (module == nullptr) {
            module = owner;
        } else {
            module_put(owner);
        }
        return true;
    };

    const bool found = resolve("screensaver_settings_load", api.loadSettings)
        && resolve("screensaver_settings_save", api.saveSettings)
        && resolve("screensaver_preview", api.preview);
    if (!found) {
        if (module != nullptr) module_put(module);
        return false;
    }
    api.module = module;
    return true;
}

void releaseModule(ModuleApi& api) {
    if (api.module != nullptr) {
        module_put(api.module);
        api.module = nullptr;
    }
}

uint32_t timeoutToIndex(uint32_t ms) {
    for (uint32_t i = 0; i < std::size(TIMEOUT_MS); i++) {
        if (TIMEOUT_MS[i] == ms) {
            return i;
        }
    }
    return 2;
}

void setHidden(lv_obj_t* object, bool hidden) {
    if (hidden) {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

void onBackPressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    app_event_emit_close(ctx->appInstanceId);
}

void onEnableSwitch(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    ctx->settings.enabled = lv_obj_has_state(ctx->enableSwitch, LV_STATE_CHECKED);
    ctx->changed = true;
    setHidden(ctx->optionsCard, !ctx->settings.enabled);
}

void onTimeoutChanged(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    ctx->settings.timeout_ms = TIMEOUT_MS[lv_dropdown_get_selected(ctx->timeoutDropdown)];
    ctx->changed = true;
}

void onTypeChanged(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    ctx->settings.type = static_cast<uint8_t>(lv_dropdown_get_selected(ctx->typeDropdown));
    ctx->changed = true;
}

void onPreviewClicked(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    task_event_group_signal(ctx->eventGroup, ctx->previewBit);
}

/** A transparent row: "Title          [content]" */
lv_obj_t* createRow(lv_obj_t* parent, const char* title) {
    auto* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(row, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_scrollable(row, false);
    auto* label = lv_label_create(row);
    lv_label_set_text(label, title);
    lv_obj_set_flex_grow(label, 1);
    return row;
}

// Called with the LVGL lock held
void createWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, LV_STATE_DEFAULT);

    lv_obj_t* toolbar = lvgl_toolbar_create(parent, "Screensaver");
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_CLOSE, onBackPressed, ctx);
    ctx->enableSwitch = lvgl_toolbar_add_switch_action(toolbar);
    if (ctx->settings.enabled) {
        lv_obj_add_state(ctx->enableSwitch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(ctx->enableSwitch, onEnableSwitch, LV_EVENT_VALUE_CHANGED, ctx);

    auto* wrapper = lv_obj_create(parent);
    lv_obj_set_style_border_width(wrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(wrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(wrapper, LV_PCT(100));
    lv_obj_set_flex_grow(wrapper, 1);

    auto* info = lv_label_create(wrapper);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(info, LV_PCT(100));
    lv_label_set_text(info, "Starts after the screen was not used for a while. Touching the screen stops it. After 5 minutes the backlight turns off.");

    ctx->optionsCard = lvgl_card_create(wrapper);
    lv_obj_set_size(ctx->optionsCard, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctx->optionsCard, LV_FLEX_FLOW_COLUMN);

    auto* timeoutRow = createRow(ctx->optionsCard, "Start after");
    ctx->timeoutDropdown = lv_dropdown_create(timeoutRow);
    lv_obj_set_width(ctx->timeoutDropdown, LV_PCT(50));
    lv_dropdown_set_options(ctx->timeoutDropdown, TIMEOUT_NAMES);
    lv_dropdown_set_selected(ctx->timeoutDropdown, timeoutToIndex(ctx->settings.timeout_ms));
    lv_obj_add_event_cb(ctx->timeoutDropdown, onTimeoutChanged, LV_EVENT_VALUE_CHANGED, ctx);

    auto* typeRow = createRow(ctx->optionsCard, "Screensaver");
    ctx->typeDropdown = lv_dropdown_create(typeRow);
    lv_obj_set_width(ctx->typeDropdown, LV_PCT(50));
    lv_dropdown_set_options(ctx->typeDropdown, TYPE_NAMES);
    lv_dropdown_set_selected(ctx->typeDropdown, ctx->settings.type);
    lv_obj_add_event_cb(ctx->typeDropdown, onTypeChanged, LV_EVENT_VALUE_CHANGED, ctx);

    auto* previewButton = lv_button_create(ctx->optionsCard);
    lv_label_set_text(lv_label_create(previewButton), "Preview");
    lv_obj_add_event_cb(previewButton, onPreviewClicked, LV_EVENT_SHORT_CLICKED, ctx);

    setHidden(ctx->optionsCard, !ctx->settings.enabled);
}

// Called with the LVGL lock held. Only the toolbar, behind the dialog that tells the module isn't running.
void createEmptyWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, LV_STATE_DEFAULT);

    lv_obj_t* toolbar = lvgl_toolbar_create(parent, "Screensaver");
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_CLOSE, onBackPressed, ctx);
}

// Called with the LVGL lock held
void destroyWidgets(void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    ctx->enableSwitch = nullptr;
    ctx->optionsCard = nullptr;
    ctx->timeoutDropdown = nullptr;
    ctx->typeDropdown = nullptr;
}

/** @return false when the dialog could not be shown */
bool showModuleMissingDialog(Context* ctx) {
    static const char* const buttons[] = { "OK" };
    return window_manager_alert_dialog(ctx->appInstanceId, "Screensaver",
        "The screensaver module is not running.\nInstall the Screensaver package again or restart the device.", buttons, 1,
        [](int32_t, void* userData) {
            auto* context = static_cast<Context*>(userData);
            task_event_group_signal(context->eventGroup, context->dialogBit);
        }, ctx) == ERROR_NONE;
}

/** Saves the settings that are shown, so a preview uses them */
void saveSettings(Context* ctx) {
    lvgl_lock();
    const ScreensaverSettings settings = ctx->settings;
    ctx->changed = false;
    lvgl_unlock();
    const error_t result = ctx->api.saveSettings(&settings);
    if (result != ERROR_NONE) {
        LOG_E(TAG, "Failed to save the settings: %s", error_to_string(result));
    }
}

}

extern "C" int main(int argc, char* argv[]) {
    Context ctx {};
    ctx.appInstanceId = app_scheduler_current_app_id();

    TaskEventGroup eventGroup {};
    task_event_group_construct(&eventGroup);
    ctx.eventGroup = &eventGroup;
    check(task_event_group_claim_bit(&eventGroup, &ctx.previewBit) == ERROR_NONE);

    check(task_event_group_claim_bit(&eventGroup, &ctx.dialogBit) == ERROR_NONE);

    AppEventSubscription subscription {};
    check(app_event_subscribe(&subscription, &eventGroup) == ERROR_NONE);

    const bool moduleAvailable = acquireModule(ctx.api);
    if (moduleAvailable) {
        ctx.api.loadSettings(&ctx.settings);
    }

    WindowId window = window_manager_create_ext(ctx.appInstanceId, moduleAvailable ? createWidgets : createEmptyWidgets, destroyWidgets, &ctx);

    bool shouldClose = false;
    if (!moduleAvailable && !showModuleMissingDialog(&ctx)) {
        shouldClose = true;
    }

    while (!shouldClose) {
        uint32_t flags = 0;
        task_event_group_wait_any(&eventGroup, &flags, portMAX_DELAY);

        // Dismissing the dialog ends the app, because there is nothing to show without the module
        if ((flags & ctx.dialogBit) != 0) {
            break;
        }

        if (moduleAvailable && (flags & ctx.previewBit) != 0) {
            saveSettings(&ctx);
            ctx.api.preview();
        }

        AppEvent event {};
        while (app_event_poll(&subscription, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                shouldClose = true;
                break;
            }
        }
    }

    lvgl_lock();
    const bool changed = ctx.changed;
    lvgl_unlock();
    if (changed && moduleAvailable) {
        saveSettings(&ctx);
    }

    window_manager_remove(window);
    check(app_event_unsubscribe(&subscription) == ERROR_NONE);
    task_event_group_release_bit(&eventGroup, ctx.previewBit);
    task_event_group_release_bit(&eventGroup, ctx.dialogBit);
    task_event_group_destruct(&eventGroup);
    releaseModule(ctx.api);

    return 0;
}
