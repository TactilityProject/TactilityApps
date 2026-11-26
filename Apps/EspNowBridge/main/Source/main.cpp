#include "EspNowBridge.h"

#include <app/event.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>

#include <memory>

extern "C" {

int main(int argc, char* argv[]) {
    AppInstanceId app_instance_id = app_scheduler_current_app_id();

    // Heap-allocated: several background tasks (OTA update, transport-wait, the WiFi event
    // callback) hold a raw Context* across the whole app instance lifetime, well past any single
    // stack frame here.
    auto ctx = std::make_unique<Context>();
    ctx->appInstanceId = app_instance_id;

    struct TaskEventGroup event_group {};
    task_event_group_construct(&event_group);

    espNowBridgeInit(ctx.get(), &event_group);

    struct AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &event_group) == ERROR_NONE);

    WindowId window = window_manager_create_ext(app_instance_id, espNowBridgeCreateWidgets, espNowBridgeDestroyWidgets, ctx.get());

    bool should_close = false;
    while (!should_close) {
        uint32_t flags = 0;
        task_event_group_wait_any(&event_group, &flags, portMAX_DELAY);

        if ((flags & ctx->fileBit) != 0 && !ctx->pickedFilePath.empty()) {
            ctx->pendingUpdateFilePath = ctx->pickedFilePath;
            espNowBridgeApplyPendingUpdate(ctx.get());
        }

        struct AppEvent event;
        while (app_event_poll(&sub, &event) == ERROR_NONE) {
            switch (event.type) {
                case APP_EVENT_CLOSE:
                    should_close = true;
                    break;

                default:
                    break;
            }
            if (should_close) break;
        }

        espNowBridgeProcessWifiEvents(ctx.get());
    }

    window_manager_remove(window);
    check(app_event_unsubscribe(&sub) == ERROR_NONE);
    // Must unsubscribe from the WiFi event group (inside espNowBridgeTeardown()) before
    // destructing it below - releasing a subscription's bit needs the group to still be alive.
    espNowBridgeTeardown(ctx.get());
    task_event_group_destruct(&event_group);

    return 0;
}

}
