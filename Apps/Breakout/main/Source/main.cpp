#include "Breakout.h"

#include <app/event.h>
#include <app/manager.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>

#include <memory>

extern "C" {

int main(int argc, char* argv[]) {
    AppInstanceId app_instance_id = app_scheduler_current_app_id();

    // Heap-allocated: Context holds several fixed-size arrays (bricks, balls, capsules,
    // per-brick state) that are too large to put on the 8192-byte app task stack (see
    // app_scheduler.cpp) alongside everything else on it.
    auto ctx = std::make_unique<Context>();
    ctx->appInstanceId = app_instance_id;

    struct AppEventSubscription sub {};
    sub.app_instance_id = app_instance_id;
    app_event_subscribe(&sub);

    WindowId window = window_manager_create(app_instance_id, breakoutCreateWidgets, ctx.get());

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
    breakoutTeardown(ctx.get());

    return 0;
}

}
