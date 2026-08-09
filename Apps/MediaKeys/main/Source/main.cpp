#include "MediaKeys.h"

#include <app/event.h>
#include <app/manager.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>

#include <memory>

extern "C" {

int main(int argc, char* argv[]) {
    AppInstanceId app_instance_id = app_scheduler_current_app_id();

    // Heap-allocated: the BT event callback (bluetooth_add_event_callback) captures ctx's
    // address for a background BT-stack thread to call back into, so it can't be a stack frame
    // that goes away while that callback might still fire.
    auto ctx = std::make_unique<Context>();
    ctx->appInstanceId = app_instance_id;

    struct AppEventSubscription sub {};
    sub.app_instance_id = app_instance_id;
    app_event_subscribe(&sub);

    WindowId window = window_manager_create(app_instance_id, mediaKeysCreateWidgets, ctx.get());
    ctx->window = window;

    bool should_close = false;
    while (!should_close) {
        struct AppEvent event;
        if (app_event_await(&sub, &event, portMAX_DELAY) != ERROR_NONE) {
            break;
        }
        if (event.type == APP_EVENT_CLOSE) {
            app_manager_finish(app_instance_id);
            should_close = true;
        }
    }

    window_manager_remove(window);
    app_event_unsubscribe(&sub);
    mediaKeysTeardown(ctx.get());

    return 0;
}

}
