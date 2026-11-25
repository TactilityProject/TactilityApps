#include "Mixer.h"

#include <app/event.h>
#include <app/paths.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>
#include <tactility/concurrent/task_event_group.h>
#include <tactility/freertos/freertos.h>
#include <tactility/freertos/task.h>

#include <memory>

extern "C" {

int main(int argc, char* argv[]) {
    auto ctx = std::make_unique<Context>();
    ctx->appInstanceId = app_scheduler_current_app_id();

    char path[192] = {};
    if (app_paths_get_user_data_directory(APP_ID, path, sizeof(path)) == ERROR_NONE) {
        ctx->songsDirectory = std::string(path) + "/songs";
    }
    if (app_paths_get_assets_directory(APP_ID, path, sizeof(path)) == ERROR_NONE) {
        ctx->examplesDirectory = std::string(path) + "/examples";
    }
    ctx->song = newSong();
    ctx->audioAvailable = ctx->audio.start(&ctx->player, "bleepmixer_audio");

    TaskEventGroup eventGroup {};
    task_event_group_construct(&eventGroup);
    AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &eventGroup) == ERROR_NONE);

    WindowId window = window_manager_create_ext(ctx->appInstanceId, mixerCreateWidgets, mixerDestroyWidgets, ctx.get());

    bool shouldClose = false;
    while (!shouldClose) {
        // Wake up regularly for file work requested by the UI
        task_event_group_wait_any(&eventGroup, nullptr, pdMS_TO_TICKS(100));
        AppEvent event {};
        while (app_event_poll(&sub, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                shouldClose = true;
            }
        }
        const Request request = ctx->request.exchange(Request::None);
        if (request != Request::None && !shouldClose) {
            runRequest(*ctx, request);
        }
    }

    ctx->player.stop();
    ctx->audio.stop();
    window_manager_remove(window);
    check(app_event_unsubscribe(&sub) == ERROR_NONE);
    task_event_group_destruct(&eventGroup);
    return 0;
}

} // extern "C"
