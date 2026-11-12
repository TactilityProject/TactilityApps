// SPDX-License-Identifier: GPL-3.0-or-later
#include "ClockSettings.h"
#include "ClockUi.h"

#include <app/event.h>
#include <app/scheduler.h>
#include <lvgl_window_manager/window_manager.h>
#include <tactility/check.h>
#include <tactility/concurrent/task_event_group.h>
#include <tactility/freertos/freertos.h>
#include <tactility/freertos/task.h>

#include <lvgl.h>

namespace {

constexpr uint32_t REFRESH_PERIOD_MS = 1000;

void onRefresh(lv_timer_t* timer) {
    auto* context = static_cast<clockapp::Context*>(lv_timer_get_user_data(timer));
    clockapp::refreshClock(context);
}

void clockCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* context = static_cast<clockapp::Context*>(userData);
    clockapp::buildUi(context, parent);
    context->timer = lv_timer_create(onRefresh, REFRESH_PERIOD_MS, context);
}

void clockDestroyWidgets(void* userData) {
    auto* context = static_cast<clockapp::Context*>(userData);
    if (context->timer != nullptr) {
        lv_timer_delete(context->timer);
        context->timer = nullptr;
    }
    clockapp::destroyUi(context);
}

}

extern "C" int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    AppInstanceId appInstanceId = app_scheduler_current_app_id();

    clockapp::Context context {};
    context.appInstanceId = appInstanceId;
    context.analog = clockapp::loadAnalogMode();

    TaskEventGroup eventGroup {};
    task_event_group_construct(&eventGroup);

    AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &eventGroup) == ERROR_NONE);

    WindowId window = window_manager_create_ext(
        appInstanceId,
        clockCreateWidgets,
        clockDestroyWidgets,
        &context
    );

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

    window_manager_remove(window);
    check(app_event_unsubscribe(&sub) == ERROR_NONE);
    task_event_group_destruct(&eventGroup);

    return 0;
}
