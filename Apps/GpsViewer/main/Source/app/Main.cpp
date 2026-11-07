// SPDX-License-Identifier: GPL-3.0-or-later
#include "AppContext.h"
#include "Sources.h"
#include "Ui.h"

#include <app/event.h>
#include <app/paths.h>
#include <app/scheduler.h>
#include <app/start.h>
#include <lvgl_window_manager/window_manager.h>
#include <tactility/Memory.hpp>
#include <tactility/check.h>
#include <tactility/time.h>

#include <freertos/task.h>
#include <sys/stat.h>

#include <cstdio>

namespace gpsviewer {

namespace {

// The app task waits this long for a GPS event before it looks at app events and requests
constexpr uint32_t POLL_MS = 50;

void settingsPath(const AppContext& context, char* out, size_t outSize) {
    snprintf(out, outSize, "%s/settings.txt", context.dataDirectory);
}

// Without a data folder the settings stay at their defaults and aren't saved
Settings loadSettings(const AppContext& context) {
    if (context.dataDirectory[0] == '\0') {
        return parseSettings("");
    }
    char path[PATH_LENGTH + 16];
    settingsPath(context, path, sizeof(path));
    char text[256] = "";
    FILE* file = fopen(path, "r");
    if (file != nullptr) {
        const size_t length = fread(text, 1, sizeof(text) - 1, file);
        text[length] = '\0';
        fclose(file);
    }
    return parseSettings(text);
}

void saveSettings(const AppContext& context, const Settings& settings) {
    if (context.dataDirectory[0] == '\0') {
        return;
    }
    char text[256];
    const size_t length = serializeSettings(settings, text, sizeof(text));
    char path[PATH_LENGTH + 16];
    settingsPath(context, path, sizeof(path));
    FILE* file = fopen(path, "w");
    if (file != nullptr) {
        fwrite(text, 1, length, file);
        fclose(file);
    }
}

// On the heap, as the recorder's write buffer alone is larger than what the app task's stack can spare
struct Sources {
    GpsSource gps;
    Recorder recorder;
};

void run(AppContext& context, Sources& sources) {
    GpsSource& gps = sources.gps;
    Recorder& recorder = sources.recorder;
    struct TaskEventGroup eventGroup {};
    task_event_group_construct(&eventGroup);
    struct AppEventSubscription appEvents {};
    check(app_event_subscribe(&appEvents, &eventGroup) == ERROR_NONE);

    gps.start(context, &eventGroup);

    WindowId window = window_manager_create_ext(context.appInstanceId, uiCreateWidgets, uiDestroyWidgets, &context);

    bool close = false;
    while (!close) {
        gps.poll(context, POLL_MS, recorder);

        struct AppEvent event;
        while (app_event_poll(&appEvents, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                close = true;
            }
        }

        {
            ContextLock lock(context);
            context.recording = recorder.isRecording();
            context.recordedLines = recorder.lineCount();
            snprintf(context.recordingName, sizeof(context.recordingName), "%s", recorder.fileName());
        }
        const uint32_t requests = context.requests.exchange(0);
        if ((requests & REQUEST_SAVE_SETTINGS) != 0) {
            Settings settings;
            {
                ContextLock lock(context);
                settings = context.settings;
            }
            saveSettings(context, settings);
        }
        if ((context.recordingToggles.exchange(0) % 2) != 0) {
            if (recorder.isRecording()) {
                recorder.stop();
            } else {
                UtcDateTime utc;
                {
                    ContextLock lock(context);
                    utc = context.model.utc();
                }
                recorder.start(context.dataDirectory, utc, get_millis());
            }
        }
        if ((requests & REQUEST_OPEN_LOCATION_SETTINGS) != 0) {
            struct AppStartContext start {};
            if (app_start_context_from_id("tactility.locationsettings", &start) == ERROR_NONE) {
                AppInstanceId instanceId;
                app_start_with_context(&start, &instanceId);
            }
        }
        if ((requests & REQUEST_RESET_SPEED) != 0) {
            ContextLock lock(context);
            context.model.resetSpeedStats();
        }
    }

    window_manager_remove(window);
    recorder.stop();
    gps.stop();
    check(app_event_unsubscribe(&appEvents) == ERROR_NONE);
    task_event_group_destruct(&eventGroup);
}

} // namespace

} // namespace gpsviewer

extern "C" int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    using namespace gpsviewer;

    // The app state is large, so it goes to external memory when there is some
    auto* context = tt::newObject<AppContext>();
    if (context == nullptr) {
        return 0;
    }
    context->appInstanceId = app_scheduler_current_app_id();
    // Without a data folder the app still shows the receiver, but keeps no settings and can't record.
    // The menu says so in place of "Start recording".
    if (app_paths_get_user_data_directory(APP_ID, context->dataDirectory, sizeof(context->dataDirectory)) == ERROR_NONE) {
        mkdir(context->dataDirectory, 0777);
    } else {
        context->dataDirectory[0] = '\0';
    }
    context->mutex = xSemaphoreCreateMutex();
    context->ui = uiAllocate(*context);
    if (context->mutex != nullptr && context->ui != nullptr) {
        context->settings = loadSettings(*context);
        context->model.reset(get_millis());
        auto* sources = tt::newObject<Sources>();
        if (sources != nullptr) {
            run(*context, *sources);
            tt::deleteObject(sources);
        }
    }
    uiFree(context->ui);
    if (context->mutex != nullptr) {
        vSemaphoreDelete(context->mutex);
    }
    tt::deleteObject(context);
    return 0;
}
