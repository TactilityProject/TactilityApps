// SPDX-License-Identifier: GPL-3.0-only
#include <webserver.h>

#include <app/event.h>
#include <app/manager.h>
#include <app/scheduler.h>
#include <app/start.h>

#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>
#include <tactility/device.h>
#include <tactility/drivers/wifi.h>
#include <tactility/log.h>
#include <tactility/module.h>

#include <lvgl.h>
#include <lvgl/grid_navigation.h>
#include <lvgl/lvgl.h>
#include <lvgl/widgets/card.h>
#include <lvgl/widgets/chip.h>
#include <lvgl/widgets/toolbar.h>

#include <atomic>
#include <cstring>
#include <string>
#include <type_traits>

namespace {

constexpr auto* TAG = "WebServerApp";

enum Request : int32_t {
    REQUEST_NONE = -1,
    REQUEST_DISABLE = 0,
    REQUEST_ENABLE = 1,
};

/**
 * The functions of the web server module. They are looked up while the app runs instead of linked,
 * so the app can start without the module and tell the user about it.
 */
struct ModuleApi {
    decltype(&webserver_settings_load) loadSettings = nullptr;
    decltype(&webserver_settings_save) saveSettings = nullptr;
    decltype(&webserver_is_enabled) isEnabled = nullptr;
    decltype(&webserver_set_enabled) setEnabled = nullptr;
    // Holds a reference, so the module can't stop while the app uses its functions
    Module* module = nullptr;
};

struct Context {
    uint32_t appInstanceId = 0;
    ModuleApi api {};
    // The dialog that tells the module isn't running, 0 when there is none
    AppInstanceId dialogId = 0;
    TaskEventGroup* eventGroup = nullptr;
    uint32_t requestBit = 0;
    // Set by the enable switch and applied by the app task, because starting the server takes a while
    std::atomic<int32_t> enableRequest { REQUEST_NONE };

    // Written by the app task and by LVGL callbacks, with the LVGL lock held
    WebServerSettings settings {};
    bool changed = false;
    // The access point mode or its settings changed, which the running server only uses after a restart
    bool accessPointChanged = false;
    // Set by the mode chips and applied by the app task, because it asks the Wi-Fi device
    std::atomic<bool> refreshAddress { false };
    // What the address card shows
    std::string address;

    // nullptr while the widgets don't exist
    lv_obj_t* enableSwitch = nullptr;
    lv_obj_t* urlCard = nullptr;
    lv_obj_t* urlLabel = nullptr;
    lv_obj_t* wifiModeChips = nullptr;
    lv_obj_t* apGroup = nullptr;
    lv_obj_t* apPasswordRow = nullptr;
    lv_obj_t* apOpenSwitch = nullptr;
    lv_obj_t* apPasswordArea = nullptr;
    lv_obj_t* authSwitch = nullptr;
    lv_obj_t* authCard = nullptr;
    lv_obj_t* usernameArea = nullptr;
    lv_obj_t* passwordArea = nullptr;
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

    const bool found = resolve("webserver_settings_load", api.loadSettings)
        && resolve("webserver_settings_save", api.saveSettings)
        && resolve("webserver_is_enabled", api.isEnabled)
        && resolve("webserver_set_enabled", api.setEnabled);
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
    const bool enabled = lv_obj_has_state(ctx->enableSwitch, LV_STATE_CHECKED);
    setHidden(ctx->urlCard, !enabled);
    ctx->enableRequest = enabled ? REQUEST_ENABLE : REQUEST_DISABLE;
    task_event_group_signal(ctx->eventGroup, ctx->requestBit);
}

void onAuthSwitch(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    ctx->settings.auth_enabled = lv_obj_has_state(ctx->authSwitch, LV_STATE_CHECKED);
    ctx->changed = true;
    setHidden(ctx->authCard, !ctx->settings.auth_enabled);
}

void onCredentialChanged(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    // Copied here, because the widgets are gone when another window covers this one
    strncpy(ctx->settings.username, lv_textarea_get_text(ctx->usernameArea), sizeof(ctx->settings.username) - 1);
    strncpy(ctx->settings.password, lv_textarea_get_text(ctx->passwordArea), sizeof(ctx->settings.password) - 1);
    ctx->changed = true;
}

/** Shows the widgets of the selected mode. Requires the LVGL lock. */
void updateVisibility(Context* ctx) {
    const bool accessPoint = ctx->settings.wifi_mode == WEBSERVER_WIFI_MODE_ACCESS_POINT;
    const uint32_t chipCount = lv_obj_get_child_count(ctx->wifiModeChips);
    for (uint32_t i = 0; i < chipCount; i++) {
        lv_obj_set_state(lv_obj_get_child(ctx->wifiModeChips, static_cast<int32_t>(i)), LV_STATE_CHECKED, (i == 1) == accessPoint);
    }
    setHidden(ctx->apGroup, !accessPoint);
    setHidden(ctx->apPasswordRow, ctx->settings.ap_open_network);
}

void onWifiModeChip(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    // Chip 0 is station mode, chip 1 is access point mode
    const uint32_t index = lv_obj_get_index(lv_event_get_target_obj(event));
    ctx->settings.wifi_mode = index == 1 ? WEBSERVER_WIFI_MODE_ACCESS_POINT : WEBSERVER_WIFI_MODE_STATION;
    ctx->changed = true;
    ctx->accessPointChanged = true;
    updateVisibility(ctx);
    ctx->refreshAddress = true;
    task_event_group_signal(ctx->eventGroup, ctx->requestBit);
}

void onApOpenSwitch(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    ctx->settings.ap_open_network = lv_obj_has_state(ctx->apOpenSwitch, LV_STATE_CHECKED);
    ctx->changed = true;
    ctx->accessPointChanged = true;
    updateVisibility(ctx);
}

void onApPasswordChanged(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    strncpy(ctx->settings.ap_password, lv_textarea_get_text(ctx->apPasswordArea), sizeof(ctx->settings.ap_password) - 1);
    ctx->changed = true;
    ctx->accessPointChanged = true;
}

lv_obj_t* createCard(lv_obj_t* parent) {
    auto* card = lvgl_card_create(parent);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    return card;
}

/** A transparent column of rows, to show or hide them together */
lv_obj_t* createGroup(lv_obj_t* parent) {
    auto* group = lv_obj_create(parent);
    lv_obj_set_size(group, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(group, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(group, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(group, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_scrollable(group, false);
    return group;
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

lv_obj_t* createTextArea(lv_obj_t* row, const char* text, uint32_t maxLength, bool password, lv_event_cb_t onChanged, Context* ctx) {
    auto* textArea = lv_textarea_create(row);
    lv_obj_set_width(textArea, LV_PCT(50));
    lv_textarea_set_one_line(textArea, true);
    lv_textarea_set_max_length(textArea, maxLength);
    lv_textarea_set_password_mode(textArea, password);
    lv_textarea_set_text(textArea, text);
    lv_obj_add_event_cb(textArea, onChanged, LV_EVENT_VALUE_CHANGED, ctx);
    return textArea;
}

// Called with the LVGL lock held
void createWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, LV_STATE_DEFAULT);

    lv_obj_t* toolbar = lvgl_toolbar_create(parent, "Web Server");
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

    // The address, while the web server is enabled
    ctx->urlCard = createCard(wrapper);
    lv_label_set_text(lv_label_create(ctx->urlCard), "Web server address");
    ctx->urlLabel = lv_label_create(ctx->urlCard);
    lv_label_set_text(ctx->urlLabel, ctx->address.c_str());
    setHidden(ctx->urlCard, !ctx->settings.enabled);

    // The Wi-Fi mode, with the access point settings in access point mode
    lv_label_set_text(lv_label_create(wrapper), "WiFi mode");
    auto* wifiCard = createCard(wrapper);
    lv_obj_set_flex_align(wifiCard, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // The chips are centered
    ctx->wifiModeChips = lv_obj_create(wifiCard);
    lv_obj_set_size(ctx->wifiModeChips, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctx->wifiModeChips, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctx->wifiModeChips, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(ctx->wifiModeChips, LV_DIR_HOR);
    lv_obj_set_style_bg_opa(ctx->wifiModeChips, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ctx->wifiModeChips, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ctx->wifiModeChips, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(ctx->wifiModeChips, 0, LV_STATE_DEFAULT);
    // In WebServerWifiMode order
    for (const char* name : { "Station", "Access point" }) {
        auto* chip = lvgl_chip_create(ctx->wifiModeChips);
        lv_label_set_text(lv_label_create(chip), name);
        lv_obj_add_event_cb(chip, onWifiModeChip, LV_EVENT_SHORT_CLICKED, ctx);
    }
    lvgl_grid_navigation_add(ctx->wifiModeChips);

    ctx->apGroup = createGroup(wifiCard);
    auto* apOpenRow = createRow(ctx->apGroup, "Open network");
    ctx->apOpenSwitch = lv_switch_create(apOpenRow);
    if (ctx->settings.ap_open_network) {
        lv_obj_add_state(ctx->apOpenSwitch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(ctx->apOpenSwitch, onApOpenSwitch, LV_EVENT_VALUE_CHANGED, ctx);

    ctx->apPasswordRow = createRow(ctx->apGroup, "Password");
    ctx->apPasswordArea = createTextArea(ctx->apPasswordRow, ctx->settings.ap_password, WEBSERVER_AP_PASSWORD_MAX_LENGTH, true, onApPasswordChanged, ctx);
    updateVisibility(ctx);

    // Authentication, with the credentials while it's required
    auto* authRow = createRow(wrapper, "Require authentication");
    ctx->authSwitch = lv_switch_create(authRow);
    if (ctx->settings.auth_enabled) {
        lv_obj_add_state(ctx->authSwitch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(ctx->authSwitch, onAuthSwitch, LV_EVENT_VALUE_CHANGED, ctx);

    ctx->authCard = createCard(wrapper);
    auto* usernameRow = createRow(ctx->authCard, "Username");
    ctx->usernameArea = createTextArea(usernameRow, ctx->settings.username, WEBSERVER_USERNAME_MAX_LENGTH, false, onCredentialChanged, ctx);
    auto* passwordRow = createRow(ctx->authCard, "Password");
    ctx->passwordArea = createTextArea(passwordRow, ctx->settings.password, WEBSERVER_PASSWORD_MAX_LENGTH, true, onCredentialChanged, ctx);
    setHidden(ctx->authCard, !ctx->settings.auth_enabled);

    auto* info = lv_label_create(wrapper);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(info, LV_PCT(100));
    lv_label_set_text(info,
        "Station mode uses the WiFi connection of the device. Use the WiFi menu to connect to a network.\n\n"
        "Access point mode makes the device a WiFi network. Its password needs 8 to 63 characters, unless the network is open. "
        "A change of these settings applies when the app closes.");
}

// Called with the LVGL lock held. Only the toolbar, behind the dialog that tells the module isn't running.
void createEmptyWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, LV_STATE_DEFAULT);

    lv_obj_t* toolbar = lvgl_toolbar_create(parent, "Web Server");
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_CLOSE, onBackPressed, ctx);
}

// Called with the LVGL lock held
void destroyWidgets(void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    ctx->enableSwitch = nullptr;
    ctx->urlCard = nullptr;
    ctx->urlLabel = nullptr;
    ctx->wifiModeChips = nullptr;
    ctx->apGroup = nullptr;
    ctx->apPasswordRow = nullptr;
    ctx->apOpenSwitch = nullptr;
    ctx->apPasswordArea = nullptr;
    ctx->authSwitch = nullptr;
    ctx->authCard = nullptr;
    ctx->usernameArea = nullptr;
    ctx->passwordArea = nullptr;
}

std::string getAddress(const WebServerSettings& settings) {
    char ip[16] = {};
    Device* wifiDevice = nullptr;
    if (device_get_first_by_type(&WIFI_TYPE, &wifiDevice) == ERROR_NONE) {
        if (settings.wifi_mode == WEBSERVER_WIFI_MODE_ACCESS_POINT) {
            // The address is fixed, also while the access point is not started
            if (wifi_access_point_get_ipv4_address(wifiDevice, ip) != ERROR_NONE) {
                strcpy(ip, "192.168.4.1");
            }
        } else {
            wifi_station_get_ipv4_address(wifiDevice, ip);
        }
        device_put(wifiDevice);
    }
    if (ip[0] == '\0') {
        return "Not connected";
    }

    std::string address = std::string("http://") + ip;
    if (settings.port != WEBSERVER_DEFAULT_PORT) {
        address += ":" + std::to_string(settings.port);
    }
    return address;
}

/** @return false when the dialog could not be started */
bool showModuleMissingDialog(Context* ctx) {
    AppStartContext start;
    if (app_start_context_from_id("tactility.alertdialog", &start) != ERROR_NONE) {
        return false;
    }
    const char* arguments[] = {
        "Web Server",
        "The web server module is not running.\nInstall the Web Server package again or restart the device.",
        "OK",
    };
    app_start_context_set_arguments_ext(&start, 3, arguments);
    app_start_context_set_parent(&start, ctx->appInstanceId);
    return app_start_with_context(&start, &ctx->dialogId) == ERROR_NONE;
}

void refreshAddress(Context* ctx) {
    lvgl_lock();
    const WebServerSettings settings = ctx->settings;
    lvgl_unlock();

    const std::string address = settings.enabled ? getAddress(settings) : "Disabled";

    lvgl_lock();
    ctx->address = address;
    if (ctx->urlLabel != nullptr) {
        lv_label_set_text(ctx->urlLabel, ctx->address.c_str());
    }
    lvgl_unlock();
}

void applyEnableRequest(Context* ctx, bool enabled) {
    // Takes a while, so the LVGL lock is not held
    WebServerSettings settings;
    lvgl_lock();
    ctx->settings.enabled = enabled;
    // Saved and applied below, so the server doesn't need a restart for them
    ctx->accessPointChanged = false;
    settings = ctx->settings;
    lvgl_unlock();

    if (ctx->api.saveSettings(&settings) != ERROR_NONE) {
        // The server still starts, only the choice isn't kept for the next boot
    }
    ctx->api.setEnabled(enabled);
    const bool running = ctx->api.isEnabled();
    const std::string address = running ? getAddress(settings) : "Disabled";

    lvgl_lock();
    ctx->address = address;
    if (ctx->urlLabel != nullptr) {
        lv_label_set_text(ctx->urlLabel, ctx->address.c_str());
    }
    // A start that failed, shows as switched off
    if (ctx->enableSwitch != nullptr && enabled != running) {
        lv_obj_set_state(ctx->enableSwitch, LV_STATE_CHECKED, running);
        if (ctx->urlCard != nullptr) setHidden(ctx->urlCard, !running);
    }
    ctx->settings.enabled = running;
    lvgl_unlock();
}

}

extern "C" int main(int argc, char* argv[]) {
    Context ctx {};
    ctx.appInstanceId = app_scheduler_current_app_id();

    TaskEventGroup eventGroup {};
    task_event_group_construct(&eventGroup);
    ctx.eventGroup = &eventGroup;
    check(task_event_group_claim_bit(&eventGroup, &ctx.requestBit) == ERROR_NONE);

    AppEventSubscription subscription {};
    check(app_event_subscribe(&subscription, &eventGroup) == ERROR_NONE);

    const bool moduleAvailable = acquireModule(ctx.api);
    if (moduleAvailable) {
        ctx.api.loadSettings(&ctx.settings);
        // Shows what the server does, in case that differs from the saved choice
        ctx.settings.enabled = ctx.api.isEnabled();
        ctx.address = ctx.settings.enabled ? getAddress(ctx.settings) : "Disabled";
    }

    WindowId window = window_manager_create_ext(ctx.appInstanceId, moduleAvailable ? createWidgets : createEmptyWidgets, destroyWidgets, &ctx);

    bool shouldClose = false;
    if (!moduleAvailable && !showModuleMissingDialog(&ctx)) {
        shouldClose = true;
    }

    while (!shouldClose) {
        task_event_group_wait_any(&eventGroup, nullptr, portMAX_DELAY);

        const int32_t request = ctx.enableRequest.exchange(REQUEST_NONE);
        if (request != REQUEST_NONE && moduleAvailable) {
            applyEnableRequest(&ctx, request == REQUEST_ENABLE);
        }
        if (ctx.refreshAddress.exchange(false) && moduleAvailable) {
            refreshAddress(&ctx);
        }

        AppEvent event {};
        while (app_event_poll(&subscription, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                shouldClose = true;
                break;
            }
            // Dismissing the dialog ends the app, because there is nothing to show without the module
            if (event.type == APP_EVENT_RESULT && event.result.launch_id == ctx.dialogId) {
                app_manager_stop(ctx.dialogId);
                ctx.dialogId = 0;
                shouldClose = true;
                break;
            }
        }
    }

    // The credentials and authentication choice are saved when the app closes
    lvgl_lock();
    const bool changed = ctx.changed;
    const bool accessPointChanged = ctx.accessPointChanged;
    const WebServerSettings settings = ctx.settings;
    lvgl_unlock();
    if (changed && moduleAvailable) {
        // Not saved when invalid (e.g. a too short access point password): the running server keeps its settings
        const error_t saveResult = ctx.api.saveSettings(&settings);
        if (saveResult != ERROR_NONE) {
            LOG_E(TAG, "Failed to save the settings: %s", error_to_string(saveResult));
        } else if (accessPointChanged && ctx.api.isEnabled()) {
            // The running server only uses a new mode or access point setting after a restart
            ctx.api.setEnabled(false);
            const error_t startResult = ctx.api.setEnabled(true);
            if (startResult != ERROR_NONE) {
                LOG_E(TAG, "Failed to restart the web server: %s", error_to_string(startResult));
            }
        }
    }

    window_manager_remove(window);
    check(app_event_unsubscribe(&subscription) == ERROR_NONE);
    task_event_group_release_bit(&eventGroup, ctx.requestBit);
    task_event_group_destruct(&eventGroup);
    releaseModule(ctx.api);

    return 0;
}
