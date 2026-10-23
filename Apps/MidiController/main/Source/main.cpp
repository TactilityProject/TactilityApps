#include "MidiController.h"

#include <app/event.h>
#include <app/manager.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>

#include <memory>

extern "C" {

int main(int argc, char* argv[]) {
    AppInstanceId app_instance_id = app_scheduler_current_app_id();

    // Heap-allocated: the BT event subscription (bluetooth_event_subscribe, see
    // MidiController::createWidgetsImpl) captures the instance's address, so it can't be a stack
    // frame that goes away while a subscription might still be active.
    auto mc = std::make_unique<MidiController>();
    mc->appInstanceId = app_instance_id;

    struct TaskEventGroup event_group {};
    task_event_group_construct(&event_group);

    struct AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &event_group) == ERROR_NONE);

    // BLE and USB MIDI start on this task: the LVGL task can't do the flash and NVS work that they need
    mc->attachEventGroup(&event_group);
    mc->detectBleState();

    WindowId window = window_manager_create(app_instance_id, MidiController::createWidgets, mc.get());
    mc->window = window;

    bool should_close = false;
    while (!should_close) {
        task_event_group_wait_any(&event_group, nullptr, portMAX_DELAY);

        struct AppEvent event;
        while (app_event_poll(&sub, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                should_close = true;
                break;
            }
        }

        if (!should_close) {
            mc->processEvents();
        }
    }

    window_manager_remove(window);
    app_event_unsubscribe(&sub);
    mc->teardown();
    task_event_group_destruct(&event_group);

    return 0;
}

}
