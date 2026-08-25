#include "MediaKeys.h"

#include <app/event.h>
#include <app/manager.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>

#include <memory>

extern "C" {

int main(int argc, char* argv[]) {
    AppInstanceId app_instance_id = app_scheduler_current_app_id();

    // Heap-allocated: BT event subscription/HID background work holds a raw Context* across the
    // whole app instance lifetime, well past any single stack frame here.
    auto ctx = std::make_unique<Context>();
    ctx->appInstanceId = app_instance_id;

    struct TaskEventGroup event_group {};
    task_event_group_construct(&event_group);
    ctx->eventGroup = &event_group;

    struct AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &event_group) == ERROR_NONE);

    // Must happen before window_manager_create() (which synchronously builds widgets and may
    // auto-enable BT) and before the wait loop below makes its first task_event_group_wait_any()
    // call - see mediaKeysInitBt()'s doc comment.
    mediaKeysInitBt(ctx.get());

    WindowId window = window_manager_create(app_instance_id, mediaKeysCreateWidgets, ctx.get());
    ctx->window = window;

    bool should_close = false;
    while (!should_close) {
        task_event_group_wait_any(&event_group, nullptr, portMAX_DELAY);

        struct AppEvent event;
        while (app_event_poll(&sub, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                should_close = true;
            }
            if (should_close) break;
        }

        mediaKeysProcessBtEvents(ctx.get());
    }

    window_manager_remove(window);
    check(app_event_unsubscribe(&sub) == ERROR_NONE);
    // Must unsubscribe from the BT event group (inside mediaKeysTeardown()) before destructing
    // it below - releasing a subscription's bit needs the group to still be alive.
    mediaKeysTeardown(ctx.get());
    task_event_group_destruct(&event_group);

    return 0;
}

}
