#include "ConnectView.h"
#include "SerialConsole.h"

#include <lvgl/lvgl.h>
#include <lvgl/widgets/toolbar.h>
#include <lvgl_window_manager/window_manager.h>

#include <app/manager.h>
#include <app/paths.h>
#include <tactility/device.h>
#include <tactility/drivers/uart_controller.h>
#include <tactility/preferences.h>

#include <cstdlib>
#include <string>

namespace {

constexpr TickType_t LVGL_DEFAULT_LOCK_TIME = 500; // 500 ticks = 500 ms

bool getPreferencesPath(std::string& outPath) {
    char path[128];
    if (app_paths_get_user_data_path("tactility.serialconsole", "serial_console.properties", path, sizeof(path)) != ERROR_NONE) {
        return false;
    }
    outPath = std::string(path);
    return true;
}

std::string buildDeviceOptions(const std::vector<Device*>& uartDevices) {
    std::string output;
    for (size_t i = 0; i < uartDevices.size(); i++) {
        if (i > 0) output.append("\n");
        output.append(uartDevices[i]->name);
    }
    return output;
}

int32_t getSpeedInput(ConnectViewState* state) {
    auto* speed_text = lv_textarea_get_text(state->speedTextarea);
    return atoi(speed_text);
}

lv_obj_t* createRowWrapper(lv_obj_t* parent) {
    auto* wrapper = lv_obj_create(parent);
    lv_obj_set_size(wrapper, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(wrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(wrapper, 0, LV_STATE_DEFAULT);
    return wrapper;
}

// Fire-and-forget: nothing in this view needs to react once the user dismisses it.
void showError(Context* app, const char* message) {
    const char* argv[] = { "Error", message, "OK" };
    uint32_t dialogInstanceId = 0;
    app_manager_start_for_result("AlertDialog", app->appInstanceId, 3, argv, &dialogInstanceId);
}

void onConnect(Context* app) {
    ConnectViewState* state = &app->connectView;

    if (!lvgl_try_lock(LVGL_DEFAULT_LOCK_TIME)) {
        return;
    }

    uint32_t selected_index = lv_dropdown_get_selected(state->busDropdown);
    if (selected_index >= state->uartDevices.size()) {
        lvgl_unlock();
        showError(app, "No UART selected");
        return;
    }

    int speed = getSpeedInput(state);
    if (speed <= 0) {
        lvgl_unlock();
        showError(app, "Invalid speed");
        return;
    }

    Device* dev = state->uartDevices[selected_index];

    UartConfig cfg = {
        (uint32_t)speed,
        UART_CONTROLLER_DATA_8_BITS,
        UART_CONTROLLER_PARITY_DISABLE,
        UART_CONTROLLER_STOP_BITS_1,
    };
    if (uart_controller_set_config(dev, &cfg) != ERROR_NONE) {
        lvgl_unlock();
        showError(app, "Failed to set baud rate");
        return;
    }

    if (uart_controller_open(dev) != ERROR_NONE) {
        lvgl_unlock();
        showError(app, "Failed to open UART");
        return;
    }

    lvgl_unlock();
    showConsoleView(app, dev);
}

void onConnectCallback(lv_event_t* event) {
    auto* app = static_cast<Context*>(lv_event_get_user_data(event));
    onConnect(app);
}

} // namespace

void connectViewCreate(lv_obj_t* parent, Context* app) {
    ConnectViewState* state = &app->connectView;

    // Enumerate UART controller devices
    state->uartDevices.clear();
    device_for_each_of_type(&UART_CONTROLLER_TYPE, &state->uartDevices, [](Device* d, void* ctx) -> bool {
        static_cast<std::vector<Device*>*>(ctx)->push_back(d);
        return true;
    });

    auto* wrapper = lv_obj_create(parent);
    lv_obj_set_flex_flow(wrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_size(wrapper, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(wrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(wrapper, 0, LV_STATE_DEFAULT);

    // Bus selection

    auto* bus_wrapper = createRowWrapper(wrapper);

    state->busDropdown = lv_dropdown_create(bus_wrapper);

    auto bus_options = buildDeviceOptions(state->uartDevices);
    lv_dropdown_set_options(state->busDropdown, bus_options.empty() ? "none" : bus_options.c_str());
    lv_obj_align(state->busDropdown, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_width(state->busDropdown, LV_PCT(50));

    std::string prefsPath;
    int32_t bus_index = 0;
    if (getPreferencesPath(prefsPath)) {
        if (Preferences* prefs = preferences_open(prefsPath.c_str())) {
            preferences_opt_int32(prefs, "bus", &bus_index);
            preferences_close(prefs);
        }
    }
    if (bus_index >= 0 && (size_t)bus_index < state->uartDevices.size()) {
        lv_dropdown_set_selected(state->busDropdown, (uint32_t)bus_index);
    }

    auto* bus_label = lv_label_create(bus_wrapper);
    lv_obj_align(bus_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_text(bus_label, "Bus");

    // Baud rate selection
    auto* baud_wrapper = createRowWrapper(wrapper);

    int32_t speed = 115200;
    if (!prefsPath.empty()) {
        if (Preferences* prefs = preferences_open(prefsPath.c_str())) {
            preferences_opt_int32(prefs, "speed", &speed);
            preferences_close(prefs);
        }
    }
    state->speedTextarea = lv_textarea_create(baud_wrapper);
    lv_textarea_set_text(state->speedTextarea, std::to_string(speed).c_str());
    lv_textarea_set_one_line(state->speedTextarea, true);
    lv_obj_set_width(state->speedTextarea, LV_PCT(50));
    lv_obj_align(state->speedTextarea, LV_ALIGN_TOP_RIGHT, 0, 0);

    auto* baud_rate_label = lv_label_create(baud_wrapper);
    lv_obj_align(baud_rate_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(baud_rate_label, "Baud");

    // Connect
    auto* connect_wrapper = createRowWrapper(wrapper);

    auto* connect_button = lv_button_create(connect_wrapper);
    lv_obj_align(connect_button, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(connect_button, onConnectCallback, LV_EVENT_SHORT_CLICKED, app);
    auto* connect_label = lv_label_create(connect_button);
    lv_label_set_text(connect_label, "Connect");
}

void connectViewStop(Context* app) {
    ConnectViewState* state = &app->connectView;

    // busDropdown/speedTextarea only exist while this window is topmost - skip reading them
    // otherwise (final app teardown runs this after window_manager_remove() already deleted
    // them; same reasoning as GPIO.cpp's periodic status timer guard).
    if (window_manager_get_state(app->window) != WINDOW_STATE_GRANTED) {
        return;
    }

    std::string prefsPath;
    if (getPreferencesPath(prefsPath)) {
        if (Preferences* prefs = preferences_open(prefsPath.c_str())) {
            int speed = getSpeedInput(state);
            if (speed > 0) {
                preferences_put_int32(prefs, "speed", speed);
            }
            auto bus_index = static_cast<int32_t>(lv_dropdown_get_selected(state->busDropdown));
            preferences_put_int32(prefs, "bus", bus_index);
            preferences_close(prefs);
        }
    }
}
