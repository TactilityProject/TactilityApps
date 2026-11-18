#include "MusicPlayer.h"

#include <app/event.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>
#include <tactility/concurrent/task_event_group.h>
#include <tactility/log.h>

extern "C" {

int main(int argc, char* argv[]) {
    MusicPlayerContext context {};
    context.appInstanceId = app_scheduler_current_app_id();

    if (!musicPlayerStartTask(&context)) {
        LOG_E(MUSIC_PLAYER_TAG, "Could not start playback task");
        std::lock_guard lock(context.statusMutex);
        context.statusText = "Could not start playback task";
    }
    musicPlayerScanFolder(&context, context.folderPath);

    TaskEventGroup eventGroup {};
    task_event_group_construct(&eventGroup);
    AppEventSubscription subscription {};
    check(app_event_subscribe(&subscription, &eventGroup) == ERROR_NONE);

    context.window = window_manager_create_ext(context.appInstanceId, musicPlayerCreateWidgets,
                                               musicPlayerDestroyWidgets, &context);

    bool shouldClose = false;
    while (!shouldClose) {
        task_event_group_wait_any(&eventGroup, nullptr, portMAX_DELAY);
        AppEvent event {};
        while (app_event_poll(&subscription, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                shouldClose = true;
                break;
            }
        }
    }

    window_manager_remove(context.window);
    context.shutdown.store(true);
    musicPlayerRequestStop(&context);
    while (context.task.load() != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    check(app_event_unsubscribe(&subscription) == ERROR_NONE);
    task_event_group_destruct(&eventGroup);
    return 0;
}

}
