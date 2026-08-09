#include "EspNowBridge.h"

#include <app/event.h>
#include <app/manager.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>

#include <tt_app_fileselection.h>

#include <memory>

extern "C" {

int main(int argc, char* argv[]) {
    AppInstanceId app_instance_id = app_scheduler_current_app_id();

    // Heap-allocated: several background tasks (OTA update, transport-wait, the WiFi event
    // callback) hold a raw Context* across the whole app instance lifetime, well past any single
    // stack frame here.
    auto ctx = std::make_unique<Context>();
    ctx->appInstanceId = app_instance_id;
    espNowBridgeInit(ctx.get());

    struct AppEventSubscription sub {};
    sub.app_instance_id = app_instance_id;
    app_event_subscribe(&sub);

    WindowId window = window_manager_create(app_instance_id, espNowBridgeCreateWidgets, ctx.get());

    bool should_close = false;
    while (!should_close) {
        struct AppEvent event;
        if (app_event_await(&sub, &event, portMAX_DELAY) != ERROR_NONE) {
            break;
        }
        switch (event.type) {
            case APP_EVENT_CLOSE:
                app_manager_finish(app_instance_id);
                should_close = true;
                break;

            case APP_EVENT_RESULT:
                if (event.result.launch_id == ctx->pickFileLaunchId) {
                    ctx->pickFileLaunchId = 0;
                    if (event.result.result == 0) { // 0 = Ok (see FileSelection.h)
                        char pathBuf[256] = {};
                        if (tt_app_fileselection_get_result_path(pathBuf, sizeof(pathBuf))) {
                            ctx->pendingUpdateFilePath = pathBuf;
                            espNowBridgeApplyPendingUpdate(ctx.get());
                        }
                    }
                }
                app_manager_stop(event.result.launch_id);
                break;

            default:
                break;
        }
    }

    window_manager_remove(window);
    app_event_unsubscribe(&sub);
    espNowBridgeTeardown(ctx.get());

    return 0;
}

}
