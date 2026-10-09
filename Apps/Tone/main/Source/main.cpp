// SPDX-License-Identifier: GPL-3.0
#include "Tone.h"

#include <app/event.h>
#include <app/manager.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>
#include <tactility/concurrent/task_event_group.h>
#include <tactility/freertos/freertos.h>
#include <tactility/freertos/task.h>

extern "C" {

int main(int argc, char* argv[]) {
    AppInstanceId appInstanceId = app_scheduler_current_app_id();

    Context ctx {};
    ctx.appInstanceId = appInstanceId;
    ctx.selectedChannel = 1;
    ctx.playback.channels = 2;

    ctx.playbackGate = xSemaphoreCreateMutex();
    check(ctx.playbackGate != nullptr);
    ctx.playback.lifecycleMutex = xSemaphoreCreateMutex();
    check(ctx.playback.lifecycleMutex != nullptr);

    TaskEventGroup eventGroup {};
    task_event_group_construct(&eventGroup);

    AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &eventGroup) == ERROR_NONE);

    WindowId window = window_manager_create_ext(appInstanceId, toneCreateWidgets, toneDestroyWidgets, &ctx);

    bool shouldClose = false;
    while (!shouldClose) {
        task_event_group_wait_any(&eventGroup, nullptr, portMAX_DELAY);

        AppEvent event {};
        while (app_event_poll(&sub, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                shouldClose = true;
                break;
            }
        }
    }

    // Block a play callback that may already be dispatching on the LVGL thread from
    // starting a new task on this context after playback stops and the window goes away.
    xSemaphoreTake(ctx.playbackGate, portMAX_DELAY);
    ctx.closing = true;
    xSemaphoreGive(ctx.playbackGate);

    // Stop playback and join the audio task before the window (and its widgets) are destroyed.
    tone_playback_stop(&ctx.playback);

    window_manager_remove(window);
    check(app_event_unsubscribe(&sub) == ERROR_NONE);
    task_event_group_destruct(&eventGroup);
    vSemaphoreDelete(ctx.playbackGate);
    vSemaphoreDelete(ctx.playback.lifecycleMutex);

    return 0;
}

} // extern "C"