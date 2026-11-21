#include "CalendarApp.h"

#include <app/event.h>
#include <app/scheduler.h>
#include <lvgl_window_manager/window_manager.h>
#include <tactility/check.h>

#include <memory>

extern "C" int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    auto context = std::make_unique<CalendarContext>();
    context->appInstanceId = app_scheduler_current_app_id();

    TaskEventGroup eventGroup {};
    task_event_group_construct(&eventGroup);
    context->eventGroup = &eventGroup;

    AppEventSubscription subscription {};
    check(app_event_subscribe(&subscription, &eventGroup) == ERROR_NONE);
    check(task_event_group_claim_bit(&eventGroup, &context->requestBit) == ERROR_NONE);

    calendarLoadSettings(context.get());
    calendarLoadCachedEvents(context.get());
    context->window = window_manager_create_ext(
        context->appInstanceId,
        calendarCreateWidgets,
        calendarDestroyWidgets,
        context.get()
    );

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

        if (!shouldClose) {
            calendarProcessRequests(context.get());
        }
    }

    window_manager_remove(context->window);
    check(app_event_unsubscribe(&subscription) == ERROR_NONE);
    task_event_group_release_bit(&eventGroup, context->requestBit);
    task_event_group_destruct(&eventGroup);
    return 0;
}
