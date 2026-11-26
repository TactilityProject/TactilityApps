#include "core/Game.h"
#include <lvgl_window_manager/dialog.h>
#include <lvgl_window_manager/window_manager.h>
#include <Bleep/AudioOutput.h>
#include "platform/CxxRuntime.h"
#include "platform/DisplayOutput.h"
#include "platform/PlatformInput.h"

#include <app/event.h>
#include <app/paths.h>
#include <app/scheduler.h>

#include <lvgl/lvgl.h>
#include <lvgl/module.h>

#include <tactility/check.h>
#include <tactility/device.h>
#include <tactility/drivers/display.h>
#include <tactility/drivers/keyboard.h>
#include <tactility/drivers/touch.h>
#include <tactility/drivers/trackball.h>
#include <tactility/freertos/freertos.h>
#include <tactility/freertos/semphr.h>
#include <tactility/freertos/task.h>
#include <tactility/log.h>
#include <tactility/module.h>
#include <tactility/time.h>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

#include <string>

constexpr auto* TAG = "Voidskim";

/** Must match manifest.properties' app.id */
static constexpr const char* APP_ID = "tactility.voidskim";

// Jet's painter sort and the game's own frames need some stack
constexpr uint32_t GAME_TASK_STACK_SIZE = 32 * 1024;

// Shows an error dialog and waits for it to close. A dialog needs a window, so an empty one is created for it.
static void showErrorAndWait(AppInstanceId appInstanceId, const char* message) {
    struct DialogState {
        TaskEventGroup eventGroup {};
        uint32_t closedBit = 0;
    } state;
    task_event_group_construct(&state.eventGroup);
    check(task_event_group_claim_bit(&state.eventGroup, &state.closedBit) == ERROR_NONE);
    AppEventSubscription subscription {};
    check(app_event_subscribe(&subscription, &state.eventGroup) == ERROR_NONE);
    const WindowId window = window_manager_create(appInstanceId, [](lv_obj_t*, void*) {}, nullptr);

    static const char* const buttons[] = { "OK" };
    const error_t result = window_manager_alert_dialog(appInstanceId, "Error", message, buttons, 1, [](int32_t, void* userData) {
        auto* dialogState = static_cast<DialogState*>(userData);
        task_event_group_signal(&dialogState->eventGroup, dialogState->closedBit);
    }, &state);

    bool done = result != ERROR_NONE;
    while (!done) {
        uint32_t flags = 0;
        task_event_group_wait_any(&state.eventGroup, &flags, portMAX_DELAY);
        done = (flags & state.closedBit) != 0;
        AppEvent event {};
        while (app_event_poll(&subscription, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                done = true;
            }
        }
    }

    window_manager_remove(window);
    check(app_event_unsubscribe(&subscription) == ERROR_NONE);
    task_event_group_release_bit(&state.eventGroup, state.closedBit);
    task_event_group_destruct(&state.eventGroup);
}

// Reads how the user sees the display from LVGL, before it's stopped.
//
// LVGL rotates either in hardware (displays with swap_xy and mirror: the panel itself is
// reconfigured, so drawing happens in the user's orientation) or in software (e.g. MIPI-DSI and RGB
// panels, which stay in their native orientation). Either way the touch driver reports native
// coordinates, which LVGL rotates itself.
static ScreenInfo getScreen(Device* display) {
    const uint32_t nativeW = display_get_resolution_x(display);
    const uint32_t nativeH = display_get_resolution_y(display);
    lv_display_t* lvglDisplay = lv_display_get_default();
    const int rotation = lvglDisplay != nullptr ? (int)lv_display_get_rotation(lvglDisplay) : 0;
    const bool hardwareRotation = display_has_capability(display, DISPLAY_CAPABILITY_CAP_SWAP_XY) &&
        display_has_capability(display, DISPLAY_CAPABILITY_CAP_MIRROR);
    const bool sideways = (rotation & 1) != 0;

    ScreenInfo screen;
    if (hardwareRotation) {
        screen.drawRotation = 0;
        screen.drawWidth = sideways ? nativeH : nativeW;
        screen.drawHeight = sideways ? nativeW : nativeH;
    } else {
        screen.drawRotation = rotation;
        screen.drawWidth = nativeW;
        screen.drawHeight = nativeH;
    }
    screen.touchRotation = rotation;
    screen.touchWidth = nativeW;
    screen.touchHeight = nativeH;
    return screen;
}

static std::string directoryOf(const char* path) {
    std::string s(path);
    const size_t slash = s.find_last_of('/');
    return slash == std::string::npos ? s : s.substr(0, slash);
}

struct GameTaskParams {
    Device* display;
    ScreenInfo screen;
    SemaphoreHandle_t done;
    std::string error;
};

static void runGame(GameTaskParams& params) {
    DisplayOutput output;
    if (!output.init(params.display, params.screen)) {
        params.error = "Could not set up the display. It may be unsupported, or memory is low.";
        output.shutdown();
        return;
    }
    PlatformInput input;
    input.init(&output);

    hd::GameConfig config;
    config.monochrome = output.isMonochrome();
    char path[192] = {};
    if (app_paths_get_assets_path(APP_ID, "vehicles.txt", path, sizeof(path)) == ERROR_NONE) {
        config.assetsDirectory = directoryOf(path);
    }
    char userPath[192] = {};
    if (app_paths_get_user_data_directory(APP_ID, userPath, sizeof(userPath)) == ERROR_NONE) {
        config.userDirectory = userPath;
        while (!config.userDirectory.empty() && config.userDirectory.back() == '/') config.userDirectory.pop_back();
    }

    {
        auto game = std::make_unique<hd::Game>();
        if (!game->init(output.frame(), output.width(), output.height(), config)) {
            params.error = game->error();
        } else {
            // Sound runs on its own task; the game only posts to it, so it never waits on audio
            input.setBindings(&game->keyBindings());
            hd::SoundSystem sound;
            game->setSound(&sound);
            bleep::AudioOutput audio;
            const bool audioStarted = audio.start(&sound, "voidskim_audio");
            hd::InputState state;
            uint32_t last = (uint32_t)get_millis();
            while (!game->quitRequested()) {
                const uint32_t now = (uint32_t)get_millis();
                const float frameTime = (float)(now - last) / 1000.0f;
                last = now;
                input.poll(state, now);
                game->update(state, frameTime);
                game->render();
                output.present(game->uiMask());
                vTaskDelay(1); // let lower priority tasks (and the watchdog) run
            }
            if (audioStarted) {
                audio.stop();
            }
            game->setSound(nullptr);
            input.setBindings(nullptr);
        }
    }

    input.shutdown();
    output.shutdown();
    runStaticDestructors();
}

static void gameTask(void* arg) {
    auto* params = static_cast<GameTaskParams*>(arg);
    runGame(*params);
    xSemaphoreGive(params->done);
    vTaskSuspend(nullptr); // deleted by main(), which also frees the stack
}

extern "C" {

int main(int argc, char* argv[]) {
    AppInstanceId appInstanceId = app_scheduler_current_app_id();

    Device* display = nullptr;
    if (device_get_first_active_by_type(&DISPLAY_TYPE, &display) != ERROR_NONE) {
        showErrorAndWait(appInstanceId, "No display device was found.");
        return 0;
    }
    if (!device_exists_of_type(&KEYBOARD_TYPE) && !device_exists_of_type(&TOUCH_TYPE) && !device_exists_of_type(&TRACKBALL_TYPE)) {
        showErrorAndWait(appInstanceId, "Voidskim needs a keyboard, trackball or touch screen.");
        device_put(display);
        return 0;
    }

    GameTaskParams params { display, getScreen(display), xSemaphoreCreateBinary(), {} };
    if (params.done == nullptr) {
        device_put(display);
        return 0;
    }

    // Take over the display
    module_stop(&lvgl_module);

    TaskHandle_t task = nullptr;
#ifdef ESP_PLATFORM
    // The stack in internal RAM when there's a block big enough, PSRAM otherwise
    const int core = portNUM_PROCESSORS > 1 ? 1 : 0;
    if (xTaskCreatePinnedToCoreWithCaps(gameTask, "voidskim", GAME_TASK_STACK_SIZE, &params, 5, &task, core, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS &&
        xTaskCreatePinnedToCoreWithCaps(gameTask, "voidskim", GAME_TASK_STACK_SIZE, &params, 5, &task, core, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        task = nullptr;
    }
#else
    if (xTaskCreate(gameTask, "voidskim", GAME_TASK_STACK_SIZE, &params, 5, &task) != pdPASS) {
        task = nullptr;
    }
#endif
    if (task != nullptr) {
        xSemaphoreTake(params.done, portMAX_DELAY);
#ifdef ESP_PLATFORM
        vTaskDeleteWithCaps(task);
#else
        vTaskDelete(task);
#endif
    } else {
        params.error = "Not enough memory to start the game.";
    }
    vSemaphoreDelete(params.done);
    device_put(display);

    if (!module_is_started(&lvgl_module)) {
        module_start(&lvgl_module);
    }
    if (!params.error.empty()) {
        LOG_E(TAG, "%s", params.error.c_str());
        showErrorAndWait(appInstanceId, params.error.c_str());
    }
    return 0;
}

}
