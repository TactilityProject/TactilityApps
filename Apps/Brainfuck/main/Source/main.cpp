#include "Brainfuck.h"

#include <app/event.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>
#include <lvgl/lvgl.h>

#include <tactility/check.h>

#include <memory>

extern "C" {

int main(int argc, char* argv[]) {
    AppInstanceId app_instance_id = app_scheduler_current_app_id();

    // Heap-allocated: Context embeds BfVM (4096-byte tape + 2048-byte output buffer, ~6.3KB) -
    // too large for the 8192-byte app task stack (see app_scheduler.cpp) alongside everything
    // else on it.
    auto ctx = std::make_unique<Context>();
    ctx->appInstanceId = app_instance_id;

    struct TaskEventGroup event_group {};
    task_event_group_construct(&event_group);

    ctx->eventGroup = &event_group;
    check(task_event_group_claim_bit(&event_group, &ctx->dialogBit) == ERROR_NONE);
    check(task_event_group_claim_bit(&event_group, &ctx->fileBit) == ERROR_NONE);

    struct AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &event_group) == ERROR_NONE);

    WindowId window = window_manager_create_ext(app_instance_id, brainfuckCreateWidgets, brainfuckDestroyWidgets, ctx.get());

    bool should_close = false;
    while (!should_close) {
        uint32_t flags = 0;
        task_event_group_wait_any(&event_group, &flags, portMAX_DELAY);

        if ((flags & ctx->dialogBit) != 0) {
            brainfuckDialogResult(ctx.get());
        }
        if ((flags & ctx->fileBit) != 0) {
            brainfuckFileResult(ctx.get());
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
    task_event_group_release_bit(&event_group, ctx->dialogBit);
    task_event_group_release_bit(&event_group, ctx->fileBit);
    task_event_group_destruct(&event_group);

    return 0;
}

}
