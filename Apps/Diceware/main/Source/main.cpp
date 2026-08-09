#include "Diceware.h"

#include <app/event.h>
#include <app/manager.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>

extern "C" {

int main(int argc, char* argv[]) {
    AppInstanceId app_instance_id = app_scheduler_current_app_id();

    Context ctx {};
    ctx.appInstanceId = app_instance_id;

    struct AppEventSubscription sub {};
    sub.app_instance_id = app_instance_id;
    app_event_subscribe(&sub);

    WindowId window = window_manager_create(app_instance_id, dicewareCreateWidgets, &ctx);

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
                if (event.result.launch_id == ctx.pendingHelpDialogId) {
                    ctx.pendingHelpDialogId = 0;
                }
                app_manager_stop(event.result.launch_id);
                break;
            default:
                break;
        }
    }

    window_manager_remove(window);
    app_event_unsubscribe(&sub);
    dicewareTeardown(&ctx);

    return 0;
}

}
