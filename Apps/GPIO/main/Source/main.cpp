#include "Gpio.h"

#include <app/event.h>
#include <app/manager.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>
#include <lvgl/lvgl.h>

#include <tactility/check.h>

extern "C" {

int main(int argc, char* argv[]) {
    AppInstanceId app_instance_id = app_scheduler_current_app_id();

    Context ctx;
    ctx.appInstanceId = app_instance_id;

    struct TaskEventGroup event_group {};
    task_event_group_construct(&event_group);

    ctx.eventGroup = &event_group;
    check(task_event_group_claim_bit(&event_group, &ctx.actionBit) == ERROR_NONE);
    gpioInit(&ctx);

    struct AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &event_group) == ERROR_NONE);

    WindowId window = window_manager_create_ext(app_instance_id, gpioCreateWidgets, gpioDestroyWidgets, &ctx);

    bool should_close = false;
    while (!should_close) {
        uint32_t flags = 0;
        task_event_group_wait_any(&event_group, &flags, portMAX_DELAY);

        if ((flags & ctx.actionBit) != 0) {
            gpioRunAction(&ctx);
        }

        struct AppEvent event;
        while (app_event_poll(&sub, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                should_close = true;
            }
            if (should_close) break;
        }
    }

    window_manager_remove(window);
    check(app_event_unsubscribe(&sub) == ERROR_NONE);
    task_event_group_release_bit(&event_group, ctx.actionBit);
    task_event_group_destruct(&event_group);
    gpioTeardown(&ctx);

    return 0;
}

}
